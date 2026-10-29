#include "vad/firered_model.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "audio/fbank.h"
#include "backends/backend_log.h"
#include "backends/ggml_header.h"
#include "errors.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"
#include "ggml.h"
#include "gguf.h"

namespace echoir::vad {
namespace {

using GgufPtr = std::unique_ptr<gguf_context, decltype(&gguf_free)>;
using GgmlPtr = std::unique_ptr<ggml_context, decltype(&ggml_free)>;
using BackendPtr = std::unique_ptr<ggml_backend, decltype(&ggml_backend_free)>;
using BufferPtr = std::unique_ptr<ggml_backend_buffer, decltype(&ggml_backend_buffer_free)>;
using GallocPtr = std::unique_ptr<ggml_gallocr, decltype(&ggml_gallocr_free)>;

constexpr const char* kArchitecture = "firered-vad";
constexpr const char* kHParamPrefix = "firered-vad.";

// Tensor shape in ggml order (ne[0] fastest), with unused dimensions set to 1.
using Shape = std::array<std::int64_t, GGML_MAX_DIMS>;

[[noreturn]] void reject(const std::string& path, const std::string& why) {
    throw ModelLoadError(path + " is not a FireRedVAD model (" + why + ")");
}

void check_architecture(const gguf_context* g, const std::string& path) {
    const std::int64_t id = gguf_find_key(g, "general.architecture");
    if (id < 0 || gguf_get_kv_type(g, id) != GGUF_TYPE_STRING ||
        std::string(gguf_get_val_str(g, id)) != kArchitecture) {
        reject(path, "general.architecture is not firered-vad");
    }
}

// The uint32 hyper-parameter firered-vad.<key>, which must lie in [lo, hi].
int read_hparam(const gguf_context* g, const std::string& path, const char* key, std::uint32_t lo,
                std::uint32_t hi) {
    const std::string full = std::string(kHParamPrefix) + key;
    const std::int64_t id = gguf_find_key(g, full.c_str());
    if (id < 0 || gguf_get_kv_type(g, id) != GGUF_TYPE_UINT32) {
        reject(path, "missing " + full);
    }
    const std::uint32_t value = gguf_get_val_u32(g, id);
    if (value < lo || value > hi) {
        reject(path, full + " " + std::to_string(value) + " is outside " + std::to_string(lo) +
                         ".." + std::to_string(hi));
    }
    return static_cast<int>(value);
}

// Every tensor's bytes must lie inside the file. A cut file would otherwise load with zeros.
void check_data_fits(const gguf_context* g, std::uint64_t file_size, const std::string& path) {
    const auto start = static_cast<std::uint64_t>(gguf_get_data_offset(g));
    if (start > file_size) {
        reject(path, "the file ends inside the tensor data");
    }
    for (std::int64_t i = 0; i < gguf_get_n_tensors(g); ++i) {
        const auto offset = static_cast<std::uint64_t>(gguf_get_tensor_offset(g, i));
        const auto size = static_cast<std::uint64_t>(gguf_get_tensor_size(g, i));
        if (offset > file_size - start || size > file_size - start - offset) {
            reject(path, "the file ends inside the tensor data");
        }
    }
}

// One copy from the file into a backend tensor. A filter is stored as [channels][taps] in the file,
// and the graph needs it as [taps][channels], so one tap of every channel is contiguous.
struct Load {
    std::uint64_t offset = 0;  // in the file
    std::size_t bytes = 0;
    ggml_tensor* target = nullptr;
    bool transpose = false;
    int channels = 0;
    int taps = 0;
};

struct Linear {
    ggml_tensor* weight = nullptr;  // ne [in, out]
    ggml_tensor* bias = nullptr;    // ne [out], or null for a layer without bias
};

// The memory of one FSMN layer: per channel, a filter over the frames before and after each frame.
struct Fsmn {
    int n1 = 0;
    int s1 = 1;
    int n2 = 0;
    int s2 = 1;
    ggml_tensor* lookback = nullptr;   // ne [P, N1]: column i holds tap i of every channel
    ggml_tensor* lookahead = nullptr;  // ne [P, N2], or null when N2 is 0
};

struct Block {
    Linear fc1;
    Linear fc2;
    Fsmn fsmn;
};

// The layers of the network, in the order the graph runs them.
struct Layers {
    Linear fc1;
    Linear fc2;
    Fsmn fsmn;
    std::vector<Block> blocks;
    std::vector<Linear> dnns;
    Linear out;
};

// Looks up the tensors the network needs. Each one must exist with the expected type and shape.
// The caller then checks that the file holds no other tensors.
class FileIndex {
public:
    FileIndex(const FireRedHParams& hp, const std::string& path, const gguf_context* gguf,
              ggml_context* weights, ggml_context* taps, std::vector<Load>& loads)
        : hp_(hp), path_(path), gguf_(gguf), weights_(weights), taps_(taps), loads_(loads) {}

    Linear linear(const std::string& prefix, std::int64_t in, std::int64_t out, bool bias) {
        Linear layer;
        layer.weight = plain(prefix + ".weight", {in, out, 1, 1});
        if (bias) {
            layer.bias = plain(prefix + ".bias", {out, 1, 1, 1});
        }
        return layer;
    }

    Fsmn fsmn(const std::string& prefix) {
        Fsmn f;
        f.n1 = hp_.N1;
        f.s1 = hp_.S1;
        f.n2 = hp_.N2;
        f.s2 = hp_.S2;
        f.lookback = filter(prefix + ".lookback_filter.weight", hp_.P, hp_.N1);
        if (hp_.N2 > 0) {
            f.lookahead = filter(prefix + ".lookahead_filter.weight", hp_.P, hp_.N2);
        }
        return f;
    }

    // A tensor copied as it is in the file.
    ggml_tensor* plain(const std::string& name, Shape shape) {
        ggml_tensor* t = expect(name, shape);
        loads_.push_back(Load{offset_of(name), ggml_nbytes(t), t, false, 0, 0});
        return t;
    }

    // A depthwise filter of channels x taps, copied transposed.
    ggml_tensor* filter(const std::string& name, int channels, int taps) {
        ggml_tensor* src = expect(name, {taps, 1, channels, 1});
        ggml_tensor* dst = ggml_new_tensor_2d(taps_, GGML_TYPE_F32, channels, taps);
        loads_.push_back(Load{offset_of(name), ggml_nbytes(src), dst, true, channels, taps});
        return dst;
    }

    // The number of tensors looked up so far.
    std::int64_t count() const { return count_; }

private:
    ggml_tensor* expect(const std::string& name, Shape shape) {
        ggml_tensor* t = ggml_get_tensor(weights_, name.c_str());
        if (t == nullptr) {
            reject(path_, "missing tensor " + name);
        }
        if (t->type != GGML_TYPE_F32 || !std::equal(shape.begin(), shape.end(), t->ne)) {
            reject(path_, "tensor " + name + " has the wrong type or shape");
        }
        ++count_;
        return t;
    }

    std::uint64_t offset_of(const std::string& name) const {
        const std::int64_t id = gguf_find_tensor(gguf_, name.c_str());
        if (id < 0) {
            reject(path_, "missing tensor " + name);
        }
        return static_cast<std::uint64_t>(gguf_get_data_offset(gguf_)) +
               static_cast<std::uint64_t>(gguf_get_tensor_offset(gguf_, id));
    }

    const FireRedHParams& hp_;
    const std::string& path_;
    const gguf_context* gguf_;
    ggml_context* weights_;
    ggml_context* taps_;
    std::vector<Load>& loads_;
    std::int64_t count_ = 0;
};

// Reads the values of one tensor from the file.
std::vector<float> read_values(std::istream& in, const Load& load, const std::string& path) {
    std::vector<float> values(load.bytes / sizeof(float));
    in.clear();
    in.seekg(static_cast<std::streamoff>(load.offset));
    in.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(load.bytes));
    if (!in || in.gcount() != static_cast<std::streamsize>(load.bytes)) {
        reject(path, "the file ends inside the tensor data");
    }
    return values;
}

// Turns a filter stored as [channels][taps] into [taps][channels].
std::vector<float> transpose_filter(const std::vector<float>& src, int channels, int taps) {
    std::vector<float> dst(src.size());
    for (int c = 0; c < channels; ++c) {
        for (int i = 0; i < taps; ++i) {
            dst[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels) +
                static_cast<std::size_t>(c)] =
                src[static_cast<std::size_t>(c) * static_cast<std::size_t>(taps) +
                    static_cast<std::size_t>(i)];
        }
    }
    return dst;
}

ggml_tensor* linear(ggml_context* ctx, const Linear& layer, ggml_tensor* x) {
    ggml_tensor* y = ggml_mul_mat(ctx, layer.weight, x);
    return layer.bias != nullptr ? ggml_add(ctx, y, layer.bias) : y;
}

// Adds the terms in pairs, level by level. A chain of adds would make the depth of the graph, and
// so the recursion of ggml's graph walk, grow with the number of taps. A thread with a 512 KB stack
// overflows that walk at a few thousand taps.
ggml_tensor* sum_pairwise(ggml_context* ctx, std::vector<ggml_tensor*> terms) {
    while (terms.size() > 1) {
        std::vector<ggml_tensor*> next;
        next.reserve((terms.size() + 1) / 2);
        for (std::size_t i = 0; i + 1 < terms.size(); i += 2) {
            next.push_back(ggml_add(ctx, terms[i], terms[i + 1]));
        }
        if (terms.size() % 2 != 0) {
            next.push_back(terms.back());
        }
        terms.swap(next);
    }
    return terms.front();
}

// The FSMN memory of p (ne [P, T]), which includes p itself. Frames outside the clip are zero, as
// the upstream convolutions pad them. A single frame gets no lookahead, as upstream does.
ggml_tensor* fsmn(ggml_context* ctx, const Fsmn& f, ggml_tensor* p) {
    const std::int64_t channels = p->ne[0];
    const std::int64_t frames = p->ne[1];
    const bool lookahead = f.n2 > 0 && frames > 1;
    const std::int64_t left = static_cast<std::int64_t>(f.n1 - 1) * f.s1;
    const std::int64_t right = lookahead ? static_cast<std::int64_t>(f.n2) * f.s2 : 0;
    ggml_tensor* padded = left + right > 0 ? ggml_pad_ext(ctx, p, 0, 0, static_cast<int>(left),
                                                          static_cast<int>(right), 0, 0, 0, 0)
                                           : p;
    std::vector<ggml_tensor*> terms = {p};

    // Adds the products of count taps. Tap i reads the padded frames from first + i * stride.
    const auto add_taps = [&](ggml_tensor* taps, int count, std::int64_t first, int stride) {
        for (int i = 0; i < count; ++i) {
            const auto at = static_cast<std::size_t>(first + i * stride) * padded->nb[1];
            ggml_tensor* window = ggml_view_2d(ctx, padded, channels, frames, padded->nb[1], at);
            const auto col = static_cast<std::size_t>(i) * taps->nb[1];
            ggml_tensor* column = ggml_view_2d(ctx, taps, channels, 1, taps->nb[1], col);
            terms.push_back(ggml_mul(ctx, window, column));
        }
    };
    add_taps(f.lookback, f.n1, 0, f.s1);
    if (lookahead) {
        // Output t reads frame t + S2 + i * S2, which is padded frame left + S2 + i * S2 + t.
        add_taps(f.lookahead, f.n2, left + f.s2, f.s2);
    }
    return sum_pairwise(ctx, std::move(terms));
}

ggml_tensor* build_network(ggml_context* ctx, const Layers& net, ggml_tensor* feats) {
    ggml_tensor* h = ggml_relu(ctx, linear(ctx, net.fc1, feats));
    ggml_tensor* p = ggml_relu(ctx, linear(ctx, net.fc2, h));
    ggml_tensor* m = fsmn(ctx, net.fsmn, p);
    for (const Block& block : net.blocks) {
        h = ggml_relu(ctx, linear(ctx, block.fc1, m));
        p = linear(ctx, block.fc2, h);
        m = ggml_add(ctx, fsmn(ctx, block.fsmn, p), m);
    }
    for (const Linear& layer : net.dnns) {
        m = ggml_relu(ctx, linear(ctx, layer, m));
    }
    return ggml_sigmoid(ctx, linear(ctx, net.out, m));
}

}  // namespace

// The backend state of a loaded model. Declared in the order the members are needed, so they are
// freed in reverse: the allocator and the buffers before the backend, and the contexts last.
struct FireRedModel::Impl {
    GgufPtr gguf{nullptr, &gguf_free};
    GgmlPtr weights{nullptr, &ggml_free};
    GgmlPtr taps{nullptr, &ggml_free};
    BackendPtr backend{nullptr, &ggml_backend_free};
    BufferPtr weights_buf{nullptr, &ggml_backend_buffer_free};
    BufferPtr taps_buf{nullptr, &ggml_backend_buffer_free};
    GallocPtr galloc{nullptr, &ggml_gallocr_free};
    Layers layers;
    std::vector<Load> loads;
    std::size_t graph_size = 0;  // bound on the nodes and the leaves of one graph
};

FireRedModel::FireRedModel(const std::filesystem::path& model, int n_threads)
    : impl_(std::make_unique<Impl>()) {
    if (n_threads < 1) {
        throw std::invalid_argument("FireRedModel: n_threads must be at least 1");
    }
    // Checked before the open, because opening a named pipe with no writer blocks.
    if (!backends::is_regular_model_file(model)) {
        throw ModelLoadError("FireRedVAD model not found: " + model.string());
    }
    backends::configure_backend_logs();
    const std::string path = model.string();
    Impl& impl = *impl_;

    // no_alloc: the file is read for its metadata and tensor layout only. The values are copied
    // into backend buffers below.
    ggml_context* weights_ctx = nullptr;
    gguf_init_params gparams{};
    gparams.no_alloc = true;
    gparams.ctx = &weights_ctx;
    impl.gguf.reset(gguf_init_from_file(path.c_str(), gparams));
    if (!impl.gguf) {
        reject(path, "not a GGUF file");
    }
    impl.weights.reset(weights_ctx);
    const gguf_context* g = impl.gguf.get();

    check_architecture(g, path);
    FireRedHParams& hp = hparams_;
    hp.idim = read_hparam(g, path, "idim", static_cast<std::uint32_t>(audio::kFbankBins),
                          static_cast<std::uint32_t>(audio::kFbankBins));
    hp.R = read_hparam(g, path, "R", 1, 64);
    hp.M = read_hparam(g, path, "M", 1, 64);
    hp.H = read_hparam(g, path, "H", 1, 4096);
    hp.P = read_hparam(g, path, "P", 1, 4096);
    hp.N1 = read_hparam(g, path, "N1", 1, 64);
    hp.S1 = read_hparam(g, path, "S1", 1, 64);
    hp.N2 = read_hparam(g, path, "N2", 0, 64);
    hp.S2 = read_hparam(g, path, "S2", 1, 64);
    hp.odim = read_hparam(g, path, "odim", 1, 1);

    std::error_code ec;
    const std::uintmax_t file_size = std::filesystem::file_size(model, ec);
    if (ec) {
        throw ModelLoadError("cannot read FireRedVAD model: " + path);
    }
    check_data_fits(g, static_cast<std::uint64_t>(file_size), path);

    impl.backend.reset(ggml_backend_cpu_init());
    if (!impl.backend) {
        throw std::runtime_error("FireRedModel: cannot start the CPU backend");
    }
    // ggml's workers spin on barriers, so more threads than cores make every step slower.
    const unsigned cores = std::thread::hardware_concurrency();
    ggml_backend_cpu_set_n_threads(
        impl.backend.get(), cores > 0 ? std::min(n_threads, static_cast<int>(cores)) : n_threads);

    // The filters are copied transposed, so they get a context and a buffer of their own.
    ggml_init_params tparams{};
    tparams.mem_size = ggml_tensor_overhead() * (2 * static_cast<std::size_t>(hp.R) + 1);
    tparams.no_alloc = true;
    impl.taps.reset(ggml_init(tparams));
    if (!impl.taps) {
        throw std::runtime_error("FireRedModel: cannot create the filter context");
    }

    FileIndex index(hp, path, g, impl.weights.get(), impl.taps.get(), impl.loads);
    Layers& net = impl.layers;
    net.fc1 = index.linear("dfsmn.fc1.0", hp.idim, hp.H, true);
    net.fc2 = index.linear("dfsmn.fc2.0", hp.H, hp.P, true);
    net.fsmn = index.fsmn("dfsmn.fsmn1");
    for (int i = 0; i < hp.R - 1; ++i) {
        const std::string base = "dfsmn.fsmns." + std::to_string(i);
        Block block;
        block.fc1 = index.linear(base + ".fc1.0", hp.P, hp.H, true);
        block.fc2 = index.linear(base + ".fc2", hp.H, hp.P, false);
        block.fsmn = index.fsmn(base + ".fsmn");
        net.blocks.push_back(block);
    }
    for (int k = 0; k < hp.M; ++k) {
        const std::string name = "dfsmn.dnns." + std::to_string(3 * k);
        net.dnns.push_back(index.linear(name, k == 0 ? hp.P : hp.H, hp.H, true));
    }
    net.out = index.linear("out", hp.H, hp.odim, true);
    ggml_tensor* cmvn_mean = index.plain("cmvn.mean", {hp.idim, 1, 1, 1});
    ggml_tensor* cmvn_istd = index.plain("cmvn.istd", {hp.idim, 1, 1, 1});
    if (gguf_get_n_tensors(g) != index.count()) {
        reject(path, "the file has tensors the network does not use");
    }

    impl.weights_buf.reset(ggml_backend_alloc_ctx_tensors(impl.weights.get(), impl.backend.get()));
    impl.taps_buf.reset(ggml_backend_alloc_ctx_tensors(impl.taps.get(), impl.backend.get()));
    if (!impl.weights_buf || !impl.taps_buf) {
        throw ModelLoadError("cannot allocate the FireRedVAD weights: " + path);
    }

    std::ifstream in(model, std::ios::binary);
    if (!in) {
        throw ModelLoadError("cannot read FireRedVAD model: " + path);
    }
    for (const Load& load : impl.loads) {
        std::vector<float> values = read_values(in, load, path);
        if (load.transpose) {
            values = transpose_filter(values, load.channels, load.taps);
        }
        ggml_backend_tensor_set(load.target, values.data(), 0, values.size() * sizeof(float));
    }

    const auto idim = static_cast<std::size_t>(hp.idim);
    cmvn_mean_.resize(idim);
    cmvn_istd_.resize(idim);
    ggml_backend_tensor_get(cmvn_mean, cmvn_mean_.data(), 0, idim * sizeof(float));
    ggml_backend_tensor_get(cmvn_istd, cmvn_istd_.data(), 0, idim * sizeof(float));

    impl.galloc.reset(ggml_gallocr_new(ggml_backend_cpu_buffer_type()));
    if (!impl.galloc) {
        throw std::runtime_error("FireRedModel: cannot create the graph allocator");
    }

    // A tap costs at most 4 nodes, and a block a few more besides its taps.
    const auto taps_per_fsmn = static_cast<std::size_t>(hp.N1 + hp.N2);
    impl.graph_size = 256 + static_cast<std::size_t>(hp.R) * (4 * taps_per_fsmn + 32) +
                      16 * static_cast<std::size_t>(hp.M);
    lookahead_ = hp.R * hp.N2 * hp.S2;
}

FireRedModel::~FireRedModel() = default;
FireRedModel::FireRedModel(FireRedModel&&) noexcept = default;
FireRedModel& FireRedModel::operator=(FireRedModel&&) noexcept = default;

std::vector<float> FireRedModel::probs(std::span<const float> feats, std::size_t frames) {
    if (!impl_) {
        throw std::runtime_error("FireRedModel: no model loaded (moved-from object)");
    }
    const auto idim = static_cast<std::size_t>(hparams_.idim);
    if (feats.size() % idim != 0 || feats.size() / idim != frames) {
        throw std::invalid_argument("FireRedModel::probs: feats must hold frames rows of " +
                                    std::to_string(idim) + " values");
    }
    if (frames == 0) {
        return {};
    }

    Impl& impl = *impl_;
    ggml_init_params gparams{};
    gparams.mem_size = ggml_tensor_overhead() * impl.graph_size +
                       ggml_graph_overhead_custom(impl.graph_size, false);
    gparams.no_alloc = true;
    GgmlPtr ctx(ggml_init(gparams), &ggml_free);
    if (!ctx) {
        throw std::runtime_error("FireRedModel: cannot create the graph context");
    }
    ggml_cgraph* graph = ggml_new_graph_custom(ctx.get(), impl.graph_size, false);
    ggml_tensor* input = ggml_new_tensor_2d(ctx.get(), GGML_TYPE_F32, hparams_.idim,
                                            static_cast<std::int64_t>(frames));
    ggml_set_input(input);
    ggml_tensor* output = build_network(ctx.get(), impl.layers, input);
    ggml_set_output(output);
    ggml_build_forward_expand(graph, output);

    if (!ggml_gallocr_alloc_graph(impl.galloc.get(), graph)) {
        throw std::runtime_error("FireRedModel: cannot allocate the graph");
    }
    ggml_backend_tensor_set(input, feats.data(), 0, feats.size_bytes());
    if (ggml_backend_graph_compute(impl.backend.get(), graph) != GGML_STATUS_SUCCESS) {
        throw std::runtime_error("FireRedModel: the graph computation failed");
    }
    std::vector<float> result(frames);
    ggml_backend_tensor_get(output, result.data(), 0, result.size() * sizeof(float));
    return result;
}

}  // namespace echoir::vad
