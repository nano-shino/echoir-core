// The FireRedVAD network: parity with the reference probabilities, the lookahead and streaming
// behaviour, and ModelLoadError or std::invalid_argument for bad input and damaged files.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <pthread.h>
#include <sys/stat.h>  // mkfifo
#endif

#include "audio/fbank.h"
#include "audio/resampler.h"
#include "audio/wav.h"
#include "doctest.h"
#include "errors.h"
#include "ggml.h"
#include "gguf.h"
#include "test_support.h"
#include "vad/firered_model.h"

namespace ea = echoir::audio;
namespace ev = echoir::vad;
namespace et = echoir::test;

namespace {

constexpr std::size_t kIdim = 80;
constexpr std::uint32_t kGgufUint32 = 4;
constexpr std::uint32_t kGgufString = 8;

// Reads a whole file of float32 values. Returns nothing if the file is missing or not whole
// floats.
std::optional<std::vector<float>> read_floats(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() % sizeof(float) != 0) {
        return std::nullopt;
    }
    std::vector<float> values(bytes.size() / sizeof(float));
    std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
}

// tests/fixtures/firered/<name>.f32, for example "jfk.vad.probs".
std::optional<std::vector<float>> golden(const std::string& name) {
    return read_floats(et::source_dir() / "tests/fixtures/firered" / (name + ".f32"));
}

std::optional<std::vector<float>> golden_feats(const std::string& clip) {
    return golden(clip + ".feat");
}

std::vector<float> load_16k_mono(const std::filesystem::path& path) {
    const ea::PcmBuffer wav = ea::read_wav(path);
    return ea::to_mono_16k(wav.samples, wav.rate, wav.channels);
}

// Largest absolute difference. Different lengths count as infinitely different.
float max_abs_diff(std::span<const float> a, std::span<const float> b) {
    if (a.size() != b.size()) {
        return std::numeric_limits<float>::infinity();
    }
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::fabs(a[i] - b[i]));
    }
    return worst;
}

// A path in the temp directory, removed when the test ends, also when an assertion fails.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string& name)
        : path(std::filesystem::temp_directory_path() / name) {}
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

std::vector<char> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
}

void write_bytes(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Little-endian, as GGUF stores its integers.
void put_u32(std::vector<char>& bytes, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes.push_back(static_cast<char>((value >> (8 * i)) & 0xffu));
    }
}

void put_u64(std::vector<char>& bytes, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<char>((value >> (8 * i)) & 0xffu));
    }
}

// A GGUF string: its length as uint64, then its characters.
std::vector<char> gguf_string(const std::string& text) {
    std::vector<char> bytes;
    put_u64(bytes, text.size());
    bytes.insert(bytes.end(), text.begin(), text.end());
    return bytes;
}

// A metadata entry with a string value: its key, its type, then the value.
std::vector<char> string_kv(const std::string& key, const std::string& value) {
    std::vector<char> bytes = gguf_string(key);
    put_u32(bytes, kGgufString);
    const std::vector<char> text = gguf_string(value);
    bytes.insert(bytes.end(), text.begin(), text.end());
    return bytes;
}

// The offset of `pattern`, which must occur exactly once in the file.
std::size_t find_once(const std::vector<char>& bytes, const std::vector<char>& pattern) {
    const auto first = std::search(bytes.begin(), bytes.end(), pattern.begin(), pattern.end());
    REQUIRE(first != bytes.end());
    const auto second = std::search(std::next(first), bytes.end(), pattern.begin(), pattern.end());
    REQUIRE(second == bytes.end());
    return static_cast<std::size_t>(first - bytes.begin());
}

// Replaces `from`, which must occur once, with `to`, which must be the same length.
std::vector<char> rewrite_once(std::vector<char> bytes, const std::vector<char>& from,
                               const std::vector<char>& to) {
    REQUIRE(from.size() == to.size());
    const std::size_t at = find_once(bytes, from);
    std::copy(to.begin(), to.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
    return bytes;
}

// Sets the uint32 metadata entry firered-vad.<key> to value. Its key and type locate the value.
std::vector<char> with_hparam(std::vector<char> bytes, const std::string& key,
                              std::uint32_t value) {
    std::vector<char> pattern = gguf_string("firered-vad." + key);
    put_u32(pattern, kGgufUint32);
    const std::size_t at = find_once(bytes, pattern) + pattern.size();
    std::vector<char> value_bytes;
    put_u32(value_bytes, value);
    std::copy(value_bytes.begin(), value_bytes.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(at));
    return bytes;
}

std::optional<std::filesystem::path> vad_model(const std::string& id) {
    return et::model_file(id, "vad");
}

// Runs the whole path from audio to probabilities, the way the VAD wrapper will.
std::vector<float> probs_from_wav(ev::FireRedModel& model, const std::filesystem::path& wav) {
    std::vector<float> feats = ea::kaldi_fbank(load_16k_mono(wav));
    ea::apply_cmvn(feats, model.cmvn_mean(), model.cmvn_istd());
    return model.probs(feats, feats.size() / kIdim);
}

// Random networks with the file's layout, for the shapes the real files do not use: strides,
// a single lookback tap, no lookahead with several DNN layers, and 64 taps. The reference below
// evaluates the formulas frame by frame, so it shares no code with the graph.

// Uniform values in [-scale, scale), from a fixed seed.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) : state_(seed) {}
    float next(float scale) {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        const double unit = static_cast<double>(state_ >> 11) / 9007199254740992.0;  // 2^53
        return static_cast<float>((2.0 * unit - 1.0) * scale);
    }

private:
    std::uint64_t state_;
};

struct SynthSpec {
    int R = 2;
    int M = 1;
    int H = 16;
    int P = 8;
    int N1 = 3;
    int S1 = 1;
    int N2 = 2;
    int S2 = 1;
};

// A linear layer. weight is [out][in] row-major; bias is [out], or empty for no bias.
struct Dense {
    std::size_t in = 0;
    std::size_t out = 0;
    std::vector<float> weight;
    std::vector<float> bias;
};

// An FSMN layer. lookback is [channels][n1] and lookahead is [channels][n2], row-major.
struct SynthFsmn {
    int n1 = 1;
    int s1 = 1;
    int n2 = 0;
    int s2 = 1;
    std::vector<float> lookback;
    std::vector<float> lookahead;
};

struct SynthBlock {
    Dense fc1;  // with bias
    Dense fc2;  // without bias
    SynthFsmn fsmn;
};

struct SynthNet {
    SynthSpec spec;
    Dense fc1;
    Dense fc2;
    SynthFsmn fsmn1;
    std::vector<SynthBlock> blocks;
    std::vector<Dense> dnns;
    Dense out;
    std::vector<float> mean;
    std::vector<float> istd;
};

SynthNet make_net(const SynthSpec& spec, std::uint64_t seed) {
    Lcg rng(seed);
    const auto dense = [&rng](std::size_t in, std::size_t out, bool bias) {
        Dense d;
        d.in = in;
        d.out = out;
        const float scale = 1.0f / std::sqrt(static_cast<float>(in));
        d.weight.resize(in * out);
        for (float& w : d.weight) {
            w = rng.next(scale);
        }
        if (bias) {
            d.bias.resize(out);
            for (float& b : d.bias) {
                b = rng.next(scale);
            }
        }
        return d;
    };
    const auto fsmn = [&rng, &spec](int n1, int s1, int n2, int s2) {
        SynthFsmn f;
        f.n1 = n1;
        f.s1 = s1;
        f.n2 = n2;
        f.s2 = s2;
        const auto channels = static_cast<std::size_t>(spec.P);
        f.lookback.resize(channels * static_cast<std::size_t>(n1));
        for (float& w : f.lookback) {
            w = rng.next(1.0f / std::sqrt(static_cast<float>(n1)));
        }
        if (n2 > 0) {
            f.lookahead.resize(channels * static_cast<std::size_t>(n2));
            for (float& w : f.lookahead) {
                w = rng.next(1.0f / std::sqrt(static_cast<float>(n2)));
            }
        }
        return f;
    };

    const auto p = static_cast<std::size_t>(spec.P);
    const auto h = static_cast<std::size_t>(spec.H);
    SynthNet net;
    net.spec = spec;
    net.fc1 = dense(kIdim, h, true);
    net.fc2 = dense(h, p, true);
    net.fsmn1 = fsmn(spec.N1, spec.S1, spec.N2, spec.S2);
    for (int i = 0; i < spec.R - 1; ++i) {
        SynthBlock block;
        block.fc1 = dense(p, h, true);
        block.fc2 = dense(h, p, false);
        block.fsmn = fsmn(spec.N1, spec.S1, spec.N2, spec.S2);
        net.blocks.push_back(std::move(block));
    }
    for (int k = 0; k < spec.M; ++k) {
        net.dnns.push_back(dense(k == 0 ? p : h, h, true));
    }
    net.out = dense(h, 1, true);
    net.mean.resize(kIdim);
    for (float& v : net.mean) {
        v = rng.next(1.0f);
    }
    net.istd.resize(kIdim);
    for (float& v : net.istd) {
        v = 1.0f + rng.next(0.5f);
    }
    return net;
}

// Writes net as a GGUF file with the names and shapes the loader reads, through ggml's writer.
// Shapes are in ggml order, so a filter is [taps][1][channels].
void write_model(const std::filesystem::path& path, const SynthNet& net) {
    const SynthSpec& s = net.spec;
    std::unique_ptr<gguf_context, decltype(&gguf_free)> gguf(gguf_init_empty(), &gguf_free);
    gguf_set_val_str(gguf.get(), "general.architecture", "firered-vad");
    const std::vector<std::pair<const char*, int>> hparams = {
        {"idim", static_cast<int>(kIdim)},
        {"R", s.R},
        {"M", s.M},
        {"H", s.H},
        {"P", s.P},
        {"N1", s.N1},
        {"S1", s.S1},
        {"N2", s.N2},
        {"S2", s.S2},
        {"odim", 1},
    };
    for (const auto& [key, value] : hparams) {
        const std::string full = std::string("firered-vad.") + key;
        gguf_set_val_u32(gguf.get(), full.c_str(), static_cast<std::uint32_t>(value));
    }

    ggml_init_params params{};
    params.mem_size = ggml_tensor_overhead() * (16 * static_cast<std::size_t>(s.R) + 64);
    params.no_alloc = true;  // only the tensor headers; the values are kept in data below
    std::unique_ptr<ggml_context, decltype(&ggml_free)> ctx(ggml_init(params), &ggml_free);
    REQUIRE(ctx != nullptr);
    std::deque<std::vector<float>> data;  // must outlive gguf_write_to_file
    const auto add = [&](const std::string& name, const std::vector<std::int64_t>& ne,
                         std::vector<float> values) {
        ggml_tensor* t =
            ggml_new_tensor(ctx.get(), GGML_TYPE_F32, static_cast<int>(ne.size()), ne.data());
        ggml_set_name(t, name.c_str());
        data.push_back(std::move(values));
        t->data = data.back().data();
        gguf_add_tensor(gguf.get(), t);
    };
    const auto linear = [&](const std::string& name, const Dense& d) {
        add(name + ".weight", {static_cast<std::int64_t>(d.in), static_cast<std::int64_t>(d.out)},
            d.weight);
        if (!d.bias.empty()) {
            add(name + ".bias", {static_cast<std::int64_t>(d.out)}, d.bias);
        }
    };
    const auto fsmn = [&](const std::string& prefix, const SynthFsmn& f) {
        const auto channels = static_cast<std::int64_t>(s.P);
        add(prefix + ".lookback_filter.weight", {f.n1, 1, channels}, f.lookback);
        if (f.n2 > 0) {
            add(prefix + ".lookahead_filter.weight", {f.n2, 1, channels}, f.lookahead);
        }
    };

    linear("dfsmn.fc1.0", net.fc1);
    linear("dfsmn.fc2.0", net.fc2);
    fsmn("dfsmn.fsmn1", net.fsmn1);
    for (std::size_t i = 0; i < net.blocks.size(); ++i) {
        const std::string base = "dfsmn.fsmns." + std::to_string(i);
        linear(base + ".fc1.0", net.blocks[i].fc1);
        linear(base + ".fc2", net.blocks[i].fc2);
        fsmn(base + ".fsmn", net.blocks[i].fsmn);
    }
    for (std::size_t k = 0; k < net.dnns.size(); ++k) {
        linear("dfsmn.dnns." + std::to_string(3 * k), net.dnns[k]);
    }
    linear("out", net.out);
    add("cmvn.mean", {static_cast<std::int64_t>(kIdim)}, net.mean);
    add("cmvn.istd", {static_cast<std::int64_t>(kIdim)}, net.istd);
    REQUIRE(gguf_write_to_file(gguf.get(), path.string().c_str(), false));
}

std::vector<float> random_feats(std::size_t frames, std::uint64_t seed) {
    Lcg rng(seed);
    std::vector<float> feats(frames * kIdim);
    for (float& v : feats) {
        v = rng.next(2.0f);
    }
    return feats;
}

// Row-major [frames][width] values.
using Rows = std::vector<float>;

Rows dense_rows(const Dense& d, const Rows& x, std::size_t frames) {
    Rows y(frames * d.out);
    for (std::size_t t = 0; t < frames; ++t) {
        for (std::size_t o = 0; o < d.out; ++o) {
            double acc = d.bias.empty() ? 0.0 : static_cast<double>(d.bias[o]);
            for (std::size_t i = 0; i < d.in; ++i) {
                acc += static_cast<double>(d.weight[o * d.in + i]) * x[t * d.in + i];
            }
            y[t * d.out + o] = static_cast<float>(acc);
        }
    }
    return y;
}

void relu_rows(Rows& x) {
    for (float& v : x) {
        v = std::max(v, 0.0f);
    }
}

// The FSMN memory of p, a [frames][channels] matrix that includes p itself. Frames outside the
// clip are zero. The lookahead applies only when the clip has more than one frame.
Rows fsmn_rows(const SynthFsmn& f, const Rows& p, std::size_t frames, std::size_t channels) {
    const auto n_frames = static_cast<std::ptrdiff_t>(frames);
    const auto value = [&](std::ptrdiff_t t, std::size_t c) {
        if (t < 0 || t >= n_frames) {
            return 0.0;
        }
        return static_cast<double>(p[static_cast<std::size_t>(t) * channels + c]);
    };
    const auto n1 = static_cast<std::ptrdiff_t>(f.n1);
    const auto n2 = static_cast<std::ptrdiff_t>(f.n2);
    const auto s1 = static_cast<std::ptrdiff_t>(f.s1);
    const auto s2 = static_cast<std::ptrdiff_t>(f.s2);
    const bool lookahead = f.n2 > 0 && frames > 1;
    Rows m(frames * channels);
    for (std::ptrdiff_t t = 0; t < n_frames; ++t) {
        for (std::size_t c = 0; c < channels; ++c) {
            double acc = value(t, c);
            for (std::ptrdiff_t i = 0; i < n1; ++i) {
                const std::ptrdiff_t at = t - (n1 - 1) * s1 + i * s1;
                acc += static_cast<double>(f.lookback[c * static_cast<std::size_t>(n1) + i]) *
                       value(at, c);
            }
            if (lookahead) {
                for (std::ptrdiff_t i = 0; i < n2; ++i) {
                    const std::ptrdiff_t at = t + s2 + i * s2;
                    acc += static_cast<double>(f.lookahead[c * static_cast<std::size_t>(n2) + i]) *
                           value(at, c);
                }
            }
            m[static_cast<std::size_t>(t) * channels + c] = static_cast<float>(acc);
        }
    }
    return m;
}

// The probabilities of the network, computed layer by layer over the whole clip.
std::vector<float> reference_probs(const SynthNet& net, const std::vector<float>& feats,
                                   std::size_t frames) {
    const std::size_t channels = static_cast<std::size_t>(net.spec.P);
    Rows h = dense_rows(net.fc1, feats, frames);
    relu_rows(h);
    Rows p = dense_rows(net.fc2, h, frames);
    relu_rows(p);
    Rows m = fsmn_rows(net.fsmn1, p, frames, channels);
    for (const SynthBlock& block : net.blocks) {
        Rows hb = dense_rows(block.fc1, m, frames);
        relu_rows(hb);
        Rows pb = dense_rows(block.fc2, hb, frames);
        Rows mb = fsmn_rows(block.fsmn, pb, frames, channels);
        for (std::size_t i = 0; i < mb.size(); ++i) {
            mb[i] += m[i];  // the block's skip connection
        }
        m = std::move(mb);
    }
    for (const Dense& layer : net.dnns) {
        m = dense_rows(layer, m, frames);
        relu_rows(m);
    }
    const Rows logits = dense_rows(net.out, m, frames);
    std::vector<float> probs(frames);
    for (std::size_t t = 0; t < frames; ++t) {
        probs[t] = 1.0f / (1.0f + std::exp(-logits[t]));
    }
    return probs;
}

#ifndef _WIN32
// probs() run on a thread with a small stack, so the depth of the graph build is tested.
struct ProbsJob {
    ev::FireRedModel* model = nullptr;
    const std::vector<float>* feats = nullptr;
    std::size_t frames = 0;
    std::vector<float> out;
    std::exception_ptr error;
};

void* run_probs(void* arg) {
    auto* job = static_cast<ProbsJob*>(arg);
    try {
        job->out = job->model->probs(*job->feats, job->frames);
    } catch (...) {
        job->error = std::current_exception();
    }
    return nullptr;
}
#endif

}  // namespace

TEST_CASE("firered models report their hyper-parameters and lookahead") {
    const auto vad_path = vad_model("firered-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(vad_path, "firered-vad-v1 (vad) model");
    const auto stream_path = vad_model("firered-stream-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(stream_path, "firered-stream-vad-v1 (vad) model");

    const ev::FireRedModel vad(*vad_path);
    const ev::FireRedHParams& hp = vad.hparams();
    CHECK(hp.idim == 80);
    CHECK(hp.R == 8);
    CHECK(hp.M == 1);
    CHECK(hp.H == 256);
    CHECK(hp.P == 128);
    CHECK(hp.N1 == 20);
    CHECK(hp.S1 == 1);
    CHECK(hp.N2 == 20);
    CHECK(hp.S2 == 1);
    CHECK(hp.odim == 1);
    // Eight FSMN layers, each reading 20 frames ahead: 160 frames in all.
    CHECK(vad.lookahead() == 160);

    const ev::FireRedModel stream(*stream_path);
    CHECK(stream.hparams().N2 == 0);
    CHECK(stream.lookahead() == 0);
}

TEST_CASE("firered synthetic networks match a direct evaluation of their formulas") {
    struct Config {
        const char* what;
        SynthSpec spec;
        std::vector<std::size_t> frames;
    };
    const std::vector<Config> configs = {
        {"strided filters with several DNN layers",
         {.R = 3, .M = 2, .H = 24, .P = 16, .N1 = 3, .S1 = 2, .N2 = 2, .S2 = 3},
         {1, 2, 3, 17, 40}},
        {"one lookback tap and no lookahead",
         {.R = 2, .M = 1, .H = 16, .P = 8, .N1 = 1, .S1 = 1, .N2 = 0, .S2 = 1},
         {1, 2, 5, 30}},
        {"no lookahead with three DNN layers",
         {.R = 2, .M = 3, .H = 12, .P = 6, .N1 = 5, .S1 = 1, .N2 = 0, .S2 = 1},
         {1, 2, 9}},
        {"a single FSMN layer with stride 3 and 2",
         {.R = 1, .M = 1, .H = 8, .P = 4, .N1 = 2, .S1 = 3, .N2 = 3, .S2 = 2},
         {1, 2, 3, 20}},
        {"64 taps each way",
         {.R = 2, .M = 1, .H = 8, .P = 4, .N1 = 64, .S1 = 1, .N2 = 64, .S2 = 1},
         {1, 2, 3, 100}},
    };
    for (const Config& c : configs) {
        INFO(c.what);
        const SynthNet net = make_net(c.spec, 1000 + static_cast<std::uint64_t>(c.spec.N1));
        TempFile file("echoir_test_firered_synthetic.gguf");
        write_model(file.path, net);
        ev::FireRedModel model(file.path, 2);
        CHECK(model.lookahead() == c.spec.R * c.spec.N2 * c.spec.S2);
        CHECK(max_abs_diff(model.cmvn_mean(), net.mean) == 0.0f);
        CHECK(max_abs_diff(model.cmvn_istd(), net.istd) == 0.0f);
        for (std::size_t frames : c.frames) {
            INFO("frames " << frames);
            const std::vector<float> feats = random_feats(frames, 7 + frames);
            const std::vector<float> got = model.probs(feats, frames);
            REQUIRE(got.size() == frames);
            CHECK(max_abs_diff(got, reference_probs(net, feats, frames)) <= 1e-4f);
        }
    }
}

TEST_CASE("firered model accepts a thread count above the core count") {
    const SynthNet net =
        make_net(SynthSpec{.R = 2, .M = 1, .H = 16, .P = 8, .N1 = 3, .S1 = 1, .N2 = 2, .S2 = 1}, 5);
    TempFile file("echoir_test_firered_threads.gguf");
    write_model(file.path, net);
    ev::FireRedModel model(file.path, 1000);
    const std::vector<float> feats = random_feats(20, 6);
    CHECK(max_abs_diff(model.probs(feats, 20), reference_probs(net, feats, 20)) <= 1e-4f);
}

#ifndef _WIN32
TEST_CASE("firered probs on a 512 KB thread stack handles 64 FSMN layers of 64 taps") {
    // 512 KB is the default stack of a secondary thread on macOS and iOS. The graph has 64 FSMN
    // layers of 128 taps each, which overflowed that stack when the taps were summed in a chain.
    const SynthSpec spec{.R = 64, .M = 1, .H = 8, .P = 4, .N1 = 64, .S1 = 1, .N2 = 64, .S2 = 1};
    const SynthNet net = make_net(spec, 77);
    TempFile file("echoir_test_firered_deep.gguf");
    write_model(file.path, net);
    ev::FireRedModel model(file.path, 2);
    const std::vector<float> feats = random_feats(4, 78);

    ProbsJob job;
    job.model = &model;
    job.feats = &feats;
    job.frames = 4;
    pthread_attr_t attr;
    REQUIRE(pthread_attr_init(&attr) == 0);
    REQUIRE(pthread_attr_setstacksize(&attr, 512 * 1024) == 0);
    pthread_t thread;
    const int created = pthread_create(&thread, &attr, run_probs, &job);
    pthread_attr_destroy(&attr);
    REQUIRE(created == 0);
    REQUIRE(pthread_join(thread, nullptr) == 0);
    REQUIRE(job.error == nullptr);
    CHECK(max_abs_diff(job.out, reference_probs(net, feats, 4)) <= 1e-4f);
}
#endif

TEST_CASE("firered CMVN statistics in the model match the golden file") {
    const auto vad_path = vad_model("firered-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(vad_path, "firered-vad-v1 (vad) model");
    const auto cmvn = read_floats(et::source_dir() / "tests/fixtures/firered/cmvn.f32");
    ECHOIR_REQUIRE_OR_SKIP(cmvn, "firered cmvn.f32 golden");
    REQUIRE(cmvn->size() == 2 * kIdim);

    const ev::FireRedModel vad(*vad_path);
    CHECK(vad.cmvn_mean().size() == kIdim);
    CHECK(vad.cmvn_istd().size() == kIdim);
    const std::span<const float> golden_mean(cmvn->data(), kIdim);
    const std::span<const float> golden_istd(cmvn->data() + kIdim, kIdim);
    CHECK(max_abs_diff(vad.cmvn_mean(), golden_mean) <= 1e-5f);
    CHECK(max_abs_diff(vad.cmvn_istd(), golden_istd) <= 1e-5f);
}

TEST_CASE("firered VAD matches the reference probabilities for jfk") {
    const auto model_path = vad_model("firered-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(model_path, "firered-vad-v1 (vad) model");
    const auto feats = golden_feats("jfk");
    ECHOIR_REQUIRE_OR_SKIP(feats, "jfk.feat.f32 golden");
    const auto expected = golden("jfk.vad.probs");
    ECHOIR_REQUIRE_OR_SKIP(expected, "jfk.vad.probs.f32 golden");

    ev::FireRedModel model(*model_path);
    const std::size_t frames = feats->size() / kIdim;
    const std::vector<float> got = model.probs(*feats, frames);
    REQUIRE(got.size() == frames);
    CHECK(max_abs_diff(got, *expected) <= 1e-4f);
}

TEST_CASE("firered Stream-VAD matches the reference probabilities for jfk") {
    const auto model_path = vad_model("firered-stream-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(model_path, "firered-stream-vad-v1 (vad) model");
    const auto feats = golden_feats("jfk");
    ECHOIR_REQUIRE_OR_SKIP(feats, "jfk.feat.f32 golden");
    const auto expected = golden("jfk.stream.probs");
    ECHOIR_REQUIRE_OR_SKIP(expected, "jfk.stream.probs.f32 golden");

    ev::FireRedModel model(*model_path);
    const std::size_t frames = feats->size() / kIdim;
    const std::vector<float> got = model.probs(*feats, frames);
    REQUIRE(got.size() == expected->size());
    CHECK(max_abs_diff(got, *expected) <= 1e-4f);
}

TEST_CASE("firered VAD matches the reference probabilities for tone_noise") {
    const auto model_path = vad_model("firered-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(model_path, "firered-vad-v1 (vad) model");
    const auto feats = golden_feats("tone_noise");
    ECHOIR_REQUIRE_OR_SKIP(feats, "tone_noise.feat.f32 golden");
    const auto expected = golden("tone_noise.vad.probs");
    ECHOIR_REQUIRE_OR_SKIP(expected, "tone_noise.vad.probs.f32 golden");

    ev::FireRedModel model(*model_path);
    const std::size_t frames = feats->size() / kIdim;
    const std::vector<float> got = model.probs(*feats, frames);
    REQUIRE(got.size() == expected->size());
    CHECK(max_abs_diff(got, *expected) <= 1e-4f);
}

TEST_CASE("firered probabilities from the audio match the reference through the front end") {
    const auto vad_path = vad_model("firered-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(vad_path, "firered-vad-v1 (vad) model");
    const auto stream_path = vad_model("firered-stream-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(stream_path, "firered-stream-vad-v1 (vad) model");

    struct Case {
        std::string wav;     // fixture name, relative to tests/fixtures
        std::string golden;  // golden probability file, without .f32
        bool stream;
    };
    const std::vector<Case> cases = {
        {"jfk", "jfk.vad.probs", false},
        {"jfk", "jfk.stream.probs", true},
        {"firered/tone_noise", "tone_noise.vad.probs", false},
    };
    for (const Case& c : cases) {
        INFO(c.wav << " -> " << c.golden);
        const auto wav = et::fixture(c.wav);
        ECHOIR_REQUIRE_OR_SKIP(wav, c.wav + ".wav fixture");
        const auto expected = golden(c.golden);
        ECHOIR_REQUIRE_OR_SKIP(expected, c.golden + ".f32 golden");

        ev::FireRedModel model(c.stream ? *stream_path : *vad_path);
        const std::vector<float> got = probs_from_wav(model, *wav);
        REQUIRE(got.size() == expected->size());
        CHECK(max_abs_diff(got, *expected) <= 5e-3f);
    }
}

TEST_CASE("firered VAD gives the reference probability for a single frame") {
    const auto model_path = vad_model("firered-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(model_path, "firered-vad-v1 (vad) model");
    const auto feats = golden_feats("jfk");
    ECHOIR_REQUIRE_OR_SKIP(feats, "jfk.feat.f32 golden");

    // A single frame has no lookahead, so this is the first frame's value on its own.
    ev::FireRedModel model(*model_path);
    const std::vector<float> got = model.probs(std::span<const float>(feats->data(), kIdim), 1);
    REQUIRE(got.size() == 1);
    CHECK(std::fabs(got[0] - 0.12654432654380798) <= 1e-4);
}

TEST_CASE("firered VAD output for a frame does not depend on frames more than lookahead() ahead") {
    const auto model_path = vad_model("firered-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(model_path, "firered-vad-v1 (vad) model");
    const auto feats = golden_feats("jfk");
    ECHOIR_REQUIRE_OR_SKIP(feats, "jfk.feat.f32 golden");

    constexpr std::size_t kPrefix = 300;
    const std::size_t frames = feats->size() / kIdim;
    REQUIRE(frames > kPrefix);
    ev::FireRedModel model(*model_path);
    const std::vector<float> full = model.probs(*feats, frames);
    const std::vector<float> prefix =
        model.probs(std::span<const float>(feats->data(), kPrefix * kIdim), kPrefix);

    // Frame t reads frames up to t + lookahead(), all inside the prefix when t < 300 - 160.
    const auto safe = kPrefix - static_cast<std::size_t>(model.lookahead());
    CHECK(max_abs_diff(std::span<const float>(prefix.data(), safe),
                       std::span<const float>(full.data(), safe)) <= 1e-5f);
}

TEST_CASE("firered Stream-VAD output for a frame does not depend on later frames") {
    const auto model_path = vad_model("firered-stream-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(model_path, "firered-stream-vad-v1 (vad) model");
    const auto feats = golden_feats("jfk");
    ECHOIR_REQUIRE_OR_SKIP(feats, "jfk.feat.f32 golden");

    constexpr std::size_t kPrefix = 300;
    const std::size_t frames = feats->size() / kIdim;
    REQUIRE(frames > kPrefix);
    ev::FireRedModel model(*model_path);
    const std::vector<float> full = model.probs(*feats, frames);
    const std::vector<float> prefix =
        model.probs(std::span<const float>(feats->data(), kPrefix * kIdim), kPrefix);
    CHECK(max_abs_diff(prefix, std::span<const float>(full.data(), kPrefix)) <= 1e-5f);
}

TEST_CASE("firered probs rejects feats that do not match the frame count") {
    const auto model_path = vad_model("firered-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(model_path, "firered-vad-v1 (vad) model");

    ev::FireRedModel model(*model_path);
    const std::vector<float> one_row(kIdim, 0.0f);
    CHECK_THROWS_AS(model.probs(one_row, 2), std::invalid_argument);
    const std::vector<float> ragged(kIdim + 1, 0.0f);
    CHECK_THROWS_AS(model.probs(ragged, 1), std::invalid_argument);
    CHECK(model.probs(std::span<const float>{}, 0).empty());
}

TEST_CASE("firered model moves keep the model and leave the source unusable") {
    const auto model_path = vad_model("firered-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(model_path, "firered-vad-v1 (vad) model");

    ev::FireRedModel first(*model_path);
    ev::FireRedModel second(std::move(first));
    CHECK(second.hparams().H == 256);
    CHECK(second.lookahead() == 160);
    const std::vector<float> one_row(kIdim, 0.0f);
    CHECK_THROWS_AS(first.probs(one_row, 1), std::runtime_error);
    CHECK(second.probs(one_row, 1).size() == 1);
}

TEST_CASE("firered model rejects n_threads below 1") {
    const auto model_path = vad_model("firered-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(model_path, "firered-vad-v1 (vad) model");
    CHECK_THROWS_AS((ev::FireRedModel{*model_path, 0}), std::invalid_argument);
}

TEST_CASE("firered model with a missing or non-model file throws ModelLoadError") {
    CHECK_THROWS_AS(ev::FireRedModel{et::source_dir() / "no-such-model.gguf"},
                    echoir::ModelLoadError);
    CHECK_THROWS_AS(ev::FireRedModel{std::filesystem::temp_directory_path()},
                    echoir::ModelLoadError);

    TempFile empty("echoir_test_firered_empty.gguf");
    write_bytes(empty.path, {});
    CHECK_THROWS_AS(ev::FireRedModel{empty.path}, echoir::ModelLoadError);

    TempFile garbage("echoir_test_firered_garbage.gguf");
    write_bytes(garbage.path, std::vector<char>(4096, 'x'));
    CHECK_THROWS_AS(ev::FireRedModel{garbage.path}, echoir::ModelLoadError);
}

#ifndef _WIN32
TEST_CASE("firered model that is a named pipe throws ModelLoadError without blocking") {
    TempFile pipe("echoir_test_firered_pipe.gguf");
    std::error_code ec;
    std::filesystem::remove(pipe.path, ec);
    REQUIRE(::mkfifo(pipe.path.c_str(), 0600) == 0);
    CHECK_THROWS_AS(ev::FireRedModel{pipe.path}, echoir::ModelLoadError);
}
#endif

TEST_CASE("firered model with damaged or inconsistent contents throws ModelLoadError") {
    const auto model_path = vad_model("firered-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(model_path, "firered-vad-v1 (vad) model");
    const std::vector<char> good = read_bytes(*model_path);
    REQUIRE(good.size() > 4096);

    const std::vector<char> arch_from = string_kv("general.architecture", "firered-vad");
    const std::vector<char> arch_to = string_kv("general.architecture", "firered-xad");

    struct Damage {
        const char* what;
        std::vector<char> bytes;
    };
    const std::vector<Damage> cases = {
        {"header cut short", std::vector<char>(good.begin(), good.begin() + 64)},
        {"data cut in half", std::vector<char>(good.begin(), good.begin() + good.size() / 2)},
        {"wrong architecture", rewrite_once(good, arch_from, arch_to)},
        {"H of zero", with_hparam(good, "H", 0)},
        {"H above the limit", with_hparam(good, "H", 5000)},
        {"idim not 80", with_hparam(good, "idim", 79)},
        {"N1 disagrees with the filter shape", with_hparam(good, "N1", 19)},
        {"N2 of zero leaves unused tensors", with_hparam(good, "N2", 0)},
        {"more DNN layers than stored", with_hparam(good, "M", 2)},
        {"more FSMN blocks than stored", with_hparam(good, "R", 9)},
        {"a hyper-parameter key missing",
         rewrite_once(good, gguf_string("firered-vad.odim"), gguf_string("firered-vad.odix"))},
        {"a tensor missing",
         rewrite_once(good, gguf_string("out.weight"), gguf_string("out.weighx"))},
    };
    for (const Damage& c : cases) {
        INFO(c.what);
        TempFile file("echoir_test_firered_damaged.gguf");
        write_bytes(file.path, c.bytes);
        CHECK_THROWS_AS(ev::FireRedModel{file.path}, echoir::ModelLoadError);
    }
}

TEST_CASE("firered VAD time for 20 s of audio with 2 threads (informational, no bound yet)") {
    const auto model_path = vad_model("firered-vad-v1");
    ECHOIR_REQUIRE_OR_SKIP(model_path, "firered-vad-v1 (vad) model");
    const auto feats = golden_feats("jfk");
    ECHOIR_REQUIRE_OR_SKIP(feats, "jfk.feat.f32 golden");

    // 20 s is 2000 frames of 10 ms. The clip's features repeat to fill that length.
    constexpr std::size_t kFrames = 2000;
    const std::size_t src_frames = feats->size() / kIdim;
    std::vector<float> input(kFrames * kIdim);
    for (std::size_t t = 0; t < kFrames; ++t) {
        std::copy_n(feats->data() + (t % src_frames) * kIdim, kIdim, input.data() + t * kIdim);
    }

    ev::FireRedModel model(*model_path, 2);
    (void)model.probs(input, kFrames);  // the first call sets up the graph
    double best_ms = std::numeric_limits<double>::infinity();
    for (int run = 0; run < 5; ++run) {
        const auto start = std::chrono::steady_clock::now();
        const std::vector<float> out = model.probs(input, kFrames);
        const auto stop = std::chrono::steady_clock::now();
        REQUIRE(out.size() == kFrames);
        best_ms =
            std::min(best_ms, std::chrono::duration<double, std::milli>(stop - start).count());
    }
    MESSAGE("firered VAD, 2000 frames (20 s), 2 threads: best of 5 = " << best_ms << " ms");
    std::fprintf(stderr, "firered VAD, 2000 frames (20 s), 2 threads: best of 5 = %.2f ms\n",
                 best_ms);
}
