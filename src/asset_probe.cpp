#include "p2_assets.h"
#include <cstdio>
#include <cstring>
#include <exception>

int main(int argc, const char* argv[]) {
    const std::filesystem::path root = argc > 1 ? argv[1] : P2_ASSET_ROOT;
    if (argc > 2) { std::fprintf(stderr, "Usage: %s [extracted-files-directory]\n", argv[0]); return 2; }
    try {
        std::size_t total = 0, models = 0, layouts = 0;
        for (const char* path : {"user/Kando/piki/pikis.szs", "new_screen/eng/title.szs"}) {
            const auto archive = p2::decompressYaz0(p2::readAsset(root, path));
            const auto resources = p2::readRarc(archive);
            std::printf("%s: %zu resources, %zu decoded bytes\n", path, resources.size(), archive.size());
            for (const auto& resource : resources) {
                const auto start = archive.begin()+resource.offset;
                const auto data = p2::decompressYaz0(p2::Bytes(start, start+resource.size));
                if (data.size() >= 4 && (!std::memcmp(data.data(), "J3D2", 4) || !std::memcmp(data.data(), "J3D1", 4))) {
                    std::vector<std::string> chunks;
                    try { chunks = p2::readJ3dChunks(data); }
                    catch (const std::exception& error) {
                        throw std::runtime_error(std::string(path) + "/" + resource.path + ": " + error.what());
                    }
                    std::printf("  J3D %-40s %zu bytes;", resource.path.c_str(), data.size());
                    for (const auto& chunk : chunks) std::printf(" %s", chunk.c_str());
                    std::printf("\n");
                    ++models;
                }
                if (data.size() >= 4 && !std::memcmp(data.data(), "SCRN", 4)) ++layouts;
                ++total;
            }
        }
        if (!models || !layouts) throw std::runtime_error("Expected both J3D resources and title screen layouts");
        std::printf("Native asset load passed: %zu resources, %zu J3D resources, %zu layouts. Audio remains disabled.\n", total, models, layouts);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Asset load failed: %s\n", error.what());
        return 1;
    }
}
