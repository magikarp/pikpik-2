#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace p2 {
using Bytes = std::vector<std::uint8_t>;
struct Resource {
    std::string path;
    std::size_t offset;
    std::size_t size;
};
// Binary offsets stay 32-bit big-endian on disk. None of these readers overlay
// a native pointer-bearing JKR/J3D struct on a file buffer.
Bytes readAsset(const std::filesystem::path& root, const std::filesystem::path& relative);
Bytes decompressYaz0(const Bytes& source);
Bytes decompressResource(const Bytes& source); // Yaz0, Yay0, or unchanged raw bytes.
std::vector<Resource> readRarc(const Bytes& archive);
std::vector<std::string> readJ3dChunks(const Bytes& model);
}
