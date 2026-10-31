// The model catalogue and file checks in src/models. These run with no model files: the tests write
// small fake files in a temporary directory.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "doctest.h"
#include "errors.h"
#include "json.hpp"
#include "models/catalogue.h"
#include "models/devices.h"
#include "models/registry.h"
#include "models/resolve.h"

namespace {

namespace fs = std::filesystem;
using echoir::models::Device;
using echoir::models::Model;
using echoir::models::Registry;

// A catalogue with fake files of known sizes, for the presence checks.
const char* kCustomRegistry = R"({
  "default_vad": {"whisper": "fake-vad", "qwen": "fake-vad"},
  "models": [
    {"id": "fake-asr", "backend": "whisper",
     "files": [{"role": "model", "name": "fake.bin", "size": 4}]},
    {"id": "fake-vad", "backend": "vad",
     "files": [{"role": "vad", "name": "fake-vad.bin"}]}
  ]
})";

// A temporary directory that is removed when the test ends.
class TempDir {
public:
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("echoir-models-test-" + std::to_string(stamp));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

    void write(const std::string& name, std::size_t bytes) const {
        std::ofstream out(path_ / name, std::ios::binary);
        const std::string data(bytes, 'x');
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

private:
    fs::path path_;
};

// The message of the exception that fn throws, or "" when it throws nothing.
template <typename Fn>
std::string error_of(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

}  // namespace

TEST_CASE("the built-in registry parses and lists whisper-tiny-en") {
    const Registry registry = Registry::builtin();
    const Model* model = registry.find("whisper-tiny-en");
    REQUIRE(model != nullptr);
    CHECK(model->backend == "whisper");
    REQUIRE(model->files.size() == 1);
    CHECK(model->files[0].role == "model");
    CHECK(model->files[0].name == "ggml-tiny.en.bin");
    CHECK(model->files[0].size == 77704715);
    CHECK(model->min_vram_mb == 390);
    CHECK(registry.find("no-such-id") == nullptr);
}

TEST_CASE("a file is present when it has the size the catalogue gives") {
    const TempDir dir;
    const Registry registry = Registry::parse(kCustomRegistry);
    const Model* model = registry.find("fake-asr");
    REQUIRE(model != nullptr);

    CHECK_FALSE(echoir::models::is_present(dir.path(), *model));  // missing
    dir.write("fake.bin", 5);
    CHECK_FALSE(echoir::models::is_present(dir.path(), *model));  // wrong size
    dir.write("fake.bin", 4);
    CHECK(echoir::models::is_present(dir.path(), *model));        // right size
    CHECK_FALSE(echoir::models::is_present(fs::path(), *model));  // no directory

    const Model* vad = registry.find("fake-vad");
    REQUIRE(vad != nullptr);
    CHECK_FALSE(echoir::models::is_present(dir.path(), *vad));
    dir.write("fake-vad.bin", 1000);  // no size given, so any size is present
    CHECK(echoir::models::is_present(dir.path(), *vad));
}

// The fake build resolves names without reading files, so these cases check the real path only.
#ifndef ECHOIR_FAKE_BACKEND
TEST_CASE("resolve names the file and the id when the file is missing or short") {
    const TempDir dir;
    const Registry registry = Registry::parse(kCustomRegistry);
    const Model& model = *registry.find("fake-asr");

    std::string missing = error_of([&] { echoir::models::resolve(dir.path(), model, "model"); });
    CHECK(missing.find("fake.bin") != std::string::npos);
    CHECK(missing.find("fake-asr") != std::string::npos);
    CHECK(missing.find("tools/fetch-models.sh fake-asr") != std::string::npos);

    dir.write("fake.bin", 2);
    std::string short_file = error_of([&] { echoir::models::resolve(dir.path(), model, "model"); });
    CHECK(short_file.find("fake.bin") != std::string::npos);
    CHECK(short_file.find("fake-asr") != std::string::npos);
    CHECK(short_file.find("2 bytes") != std::string::npos);

    dir.write("fake.bin", 4);
    CHECK(echoir::models::resolve(dir.path(), model, "model") == dir.path() / "fake.bin");
}

TEST_CASE("resolve throws a model load error for a missing or short file") {
    const TempDir dir;
    const Registry registry = Registry::parse(kCustomRegistry);
    const Model& model = *registry.find("fake-asr");
    CHECK_THROWS_AS(echoir::models::resolve(dir.path(), model, "model"), echoir::ModelLoadError);
    dir.write("fake.bin", 3);
    CHECK_THROWS_AS(echoir::models::resolve(dir.path(), model, "model"), echoir::ModelLoadError);
}
#endif

TEST_CASE("resolve rejects a role the entry does not have") {
    const Registry registry = Registry::builtin();
    const Model& model = *registry.find("whisper-tiny-en");
    CHECK_THROWS_AS(echoir::models::resolve(fs::path("."), model, "mmproj"), std::invalid_argument);
}

TEST_CASE("default VADs are per backend key, and speech backends map to a key") {
    const Registry registry = Registry::builtin();
    CHECK(registry.default_vad("whisper") == "silero-v6.2.0");
    CHECK(registry.default_vad("qwen") == "firered-vad-v1");
    CHECK_THROWS_AS(registry.default_vad("no-such-backend"), std::runtime_error);

    CHECK(echoir::models::backend_key("whisper") == "whisper");
    CHECK(echoir::models::backend_key("qwen3-asr") == "qwen");
    CHECK(echoir::models::backend_key("vad").empty());
}

TEST_CASE("a custom catalogue replaces the built-in one") {
    const Registry registry = Registry::parse(kCustomRegistry);
    CHECK(registry.find("fake-asr") != nullptr);
    CHECK(registry.find("whisper-tiny-en") == nullptr);
    CHECK(registry.default_vad("whisper") == "fake-vad");
    CHECK(registry.models().size() == 2);
}

TEST_CASE("text that is not a catalogue is rejected") {
    CHECK_THROWS_AS(Registry::parse("{"), std::invalid_argument);
    CHECK_THROWS_AS(Registry::parse(R"({"models": []})"), std::invalid_argument);
}

TEST_CASE("the catalogue JSON reports presence, default VADs and fit per device") {
    const TempDir dir;
    dir.write("fake.bin", 4);
    const Registry registry = Registry::parse(kCustomRegistry);
    // A GPU with 1000 MB free and a larger CPU. The best device is the GPU.
    const std::vector<Device> devices = {
        {"cpu", false, 16000, 8000},
        {"gpu", true, 8000, 1000, "Test GPU", false},
    };
    const nlohmann::json doc =
        nlohmann::json::parse(echoir::models::catalogue_json(registry, dir.path(), devices));

    REQUIRE(doc.at("devices").size() == 2);
    CHECK(doc.at("devices")[0].at("type") == "cpu");
    CHECK(doc.at("devices")[1].at("type") == "gpu");
    CHECK(doc.at("devices")[1].at("memory_free_mb") == 1000);
    CHECK(doc.at("devices")[1].at("description") == "Test GPU");
    CHECK(doc.at("devices")[0].at("integrated") == false);
    CHECK(doc.at("devices")[1].at("integrated") == false);
    CHECK(doc.at("default_vad").at("whisper") == "fake-vad");

    const nlohmann::json& asr = doc.at("models")[0];
    CHECK(asr.at("id") == "fake-asr");
    CHECK(asr.at("present") == true);
    CHECK(asr.at("files")[0].at("present") == true);
    CHECK(asr.at("fits") == true);                      // no min_vram_mb: fits
    CHECK(doc.at("models")[1].at("present") == false);  // vad file not written

    const Registry builtin = Registry::builtin();
    const nlohmann::json catalogue =
        nlohmann::json::parse(echoir::models::catalogue_json(builtin, fs::path(), devices));
    for (const nlohmann::json& model : catalogue.at("models")) {
        CHECK(model.at("present") == false);  // no directory: nothing is present
        if (model.at("id") == "whisper-tiny-en") {
            CHECK(model.at("fits") == true);  // 390 MB fits in 1000 MB on the GPU
        }
        if (model.at("id") == "qwen3-asr-1.7b-q8_0") {
            CHECK(model.at("fits") == false);  // 3500 MB does not, even though the CPU has room
        }
    }
}

TEST_CASE("with no devices, only entries that need no memory fit") {
    const Registry builtin = Registry::builtin();
    const nlohmann::json catalogue = nlohmann::json::parse(
        echoir::models::catalogue_json(builtin, fs::path(), std::vector<Device>{}));
    CHECK(catalogue.at("devices").empty());
    for (const nlohmann::json& model : catalogue.at("models")) {
        if (model.at("id") == "silero-v6.2.0") {
            CHECK(model.at("fits") == true);
        }
        if (model.at("id") == "whisper-tiny-en") {
            CHECK(model.at("fits") == false);
        }
    }
}
