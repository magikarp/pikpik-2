#include "p2_assets.h"
#include <cstring>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace p2 {
namespace {
constexpr std::size_t MaxAssetSize = 256 * 1024 * 1024;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void range(const Bytes& bytes, std::size_t start, std::size_t count) {
    require(start <= bytes.size() && count <= bytes.size() - start, "Resource range outside buffer");
}
std::uint32_t be32(const Bytes& bytes, std::size_t pos) {
    range(bytes, pos, 4);
    return (std::uint32_t(bytes[pos]) << 24) | (std::uint32_t(bytes[pos+1]) << 16)
        | (std::uint32_t(bytes[pos+2]) << 8) | bytes[pos+3];
}
std::uint16_t be16(const Bytes& bytes, std::size_t pos) {
    range(bytes, pos, 2);
    return (std::uint16_t(bytes[pos]) << 8) | bytes[pos+1];
}
bool magic(const Bytes& bytes, const char* tag) {
    return bytes.size() >= 4 && std::memcmp(bytes.data(), tag, 4) == 0;
}
}

Bytes readAsset(const std::filesystem::path& root, const std::filesystem::path& relative) {
    require(!relative.empty() && !relative.is_absolute(), "Asset path must be relative to the disc root");
    const auto base = std::filesystem::canonical(root);
    const auto file = std::filesystem::canonical(base / relative);
    const auto normalized = file.lexically_relative(base);
    require(!normalized.empty() && *normalized.begin() != "..", "Asset path escapes the disc root");
    const auto size = std::filesystem::file_size(file);
    require(size <= MaxAssetSize, "Asset exceeds the bring-up size limit");
    Bytes bytes(static_cast<std::size_t>(size));
    std::ifstream input(file, std::ios::binary);
    require(bool(input), "Cannot open asset");
    require(bool(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), "Incomplete asset read");
    return bytes;
}

Bytes decompressYaz0(const Bytes& source) {
    if (!magic(source, "Yaz0")) return source;
    range(source, 0, 16);
    const auto length = be32(source, 4);
    require(length <= MaxAssetSize, "Yaz0 decoded size exceeds the bring-up limit");
    Bytes result;
    result.reserve(length);
    std::size_t pos = 16;
    while (result.size() < length) {
        range(source, pos, 1);
        const auto flags = source[pos++];
        for (unsigned mask = 0x80; mask && result.size() < length; mask >>= 1) {
            if (flags & mask) {
                range(source, pos, 1);
                result.push_back(source[pos++]);
            } else {
                range(source, pos, 2);
                const auto first = source[pos++];
                const auto second = source[pos++];
                const std::size_t distance = (((first & 15) << 8) | second) + 1;
                std::size_t count = first >> 4;
                if (!count) {
                    range(source, pos, 1);
                    count = source[pos++] + 18;
                } else count += 2;
                require(distance <= result.size(), "Invalid Yaz0 backward reference");
                require(count <= length - result.size(), "Yaz0 run exceeds decoded size");
                // Overlapping copies are intentional: the source advances into
                // bytes written by this very run.
                for (std::size_t i = 0; i < count; ++i) result.push_back(result[result.size()-distance]);
            }
        }
    }
    return result;
}

Bytes decompressResource(const Bytes& source) {
    if (!magic(source, "Yay0")) return decompressYaz0(source);
    range(source, 0, 16);
    const auto length = be32(source, 4);
    require(length <= MaxAssetSize, "Yay0 decoded size exceeds the bring-up limit");
    std::size_t link = be32(source, 8), data = be32(source, 12), flagsAt = 16;
    const auto linksBegin = link, dataBegin = data;
    require(16 <= link && link <= data && data <= source.size(), "Invalid Yay0 stream offsets");
    Bytes result;
    result.reserve(length);
    std::uint32_t flags = 0, mask = 0;
    while (result.size() < length) {
        if (!mask) {
            require(flagsAt <= linksBegin && linksBegin-flagsAt >= 4, "Truncated Yay0 flag stream");
            flags = be32(source, flagsAt); flagsAt += 4; mask = 0x80000000u;
        }
        if (flags & mask) {
            range(source, data, 1); result.push_back(source[data++]);
        } else {
            require(link <= dataBegin && dataBegin-link >= 2, "Truncated Yay0 link stream");
            const auto pair = be16(source, link); link += 2;
            const std::size_t distance = (pair & 0xfff)+1;
            std::size_t count = pair >> 12;
            if (!count) { range(source, data, 1); count = source[data++]+18; }
            else count += 2;
            require(distance <= result.size(), "Invalid Yay0 backward reference");
            // JKRDecomp::decodeSZP clips the terminal run to the declared size.
            if (count > length-result.size()) count = length-result.size();
            for (std::size_t i=0; i<count; ++i) result.push_back(result[result.size()-distance]);
        }
        mask >>= 1;
    }
    return result;
}

std::vector<Resource> readRarc(const Bytes& archive) {
    range(archive, 0, 64);
    require(magic(archive, "RARC"), "Expected a RARC archive");
    const auto fileSize = be32(archive, 4);
    require(fileSize == archive.size(), "RARC file length mismatch");
    const std::size_t base = be32(archive, 8);
    require(base == 32, "Unsupported RARC header length");
    const std::size_t data = base + be32(archive, 12);
    const std::size_t dataSize = be32(archive, 16);
    range(archive, data, dataSize);
    const auto nodes = be32(archive, base);
    const std::size_t nodeStart = base + be32(archive, base+4);
    const auto entries = be32(archive, base+8);
    const std::size_t entryStart = base + be32(archive, base+12);
    const auto stringsSize = be32(archive, base+16);
    const std::size_t strings = base + be32(archive, base+20);
    range(archive, nodeStart, std::size_t(nodes)*16);
    range(archive, entryStart, std::size_t(entries)*20);
    range(archive, strings, stringsSize);
    require(nodes > 0, "RARC contains no root directory");
    auto nameAt = [&](std::uint32_t offset) {
        require(offset < stringsSize, "RARC name offset outside string table");
        const char* start = reinterpret_cast<const char*>(archive.data()+strings+offset);
        const void* end = std::memchr(start, 0, stringsSize-offset);
        require(end != nullptr, "Unterminated RARC resource name");
        std::string name(start, static_cast<const char*>(end));
        require(!name.empty() && name.find('/') == std::string::npos && name.find('\\') == std::string::npos,
                "Invalid RARC path component");
        return name;
    };
    std::vector<Resource> result;
    std::vector<bool> visited(nodes, false);
    std::function<void(std::uint32_t, const std::string&, unsigned)> visit;
    visit = [&](std::uint32_t node, const std::string& prefix, unsigned depth) {
        require(node < nodes && !visited[node] && depth < 128, "Invalid or cyclic RARC directory");
        visited[node] = true;
        const auto address = nodeStart + std::size_t(node)*16;
        const auto count = be16(archive, address+10);
        const auto first = be32(archive, address+12);
        require(first <= entries && count <= entries-first, "RARC directory range invalid");
        for (std::uint32_t i = first; i < first+count; ++i) {
            const auto entry = entryStart + std::size_t(i)*20;
            const auto flagsAndName = be32(archive, entry+4);
            const auto name = nameAt(flagsAndName & 0xffffff);
            if (name == "." || name == "..") continue;
            const auto offset = be32(archive, entry+8);
            const auto size = be32(archive, entry+12);
            const auto path = prefix.empty() ? name : prefix + "/" + name;
            if ((flagsAndName >> 24) & 2) visit(offset, path, depth+1);
            else {
                require(offset <= dataSize && size <= dataSize-offset, "RARC file exceeds data region");
                result.push_back({path, data+offset, size});
            }
        }
    };
    visit(0, "", 0);
    return result;
}

std::vector<std::string> readJ3dChunks(const Bytes& model) {
    range(model, 0, 32);
    require(magic(model, "J3D2") || magic(model, "J3D1"), "Expected a J3D resource");
    const std::size_t length = be32(model, 8);
    const auto count = be32(model, 12);
    // Retail BCA entries omit final 32-byte alignment padding even though their
    // J3D header retains the padded length. Only tolerate that terminal padding;
    // all headers and subsequent payload reads are bounded by actual bytes.
    const auto paddedSize = (model.size()+31) & ~std::size_t(31);
    require(length <= paddedSize && length >= 32, "J3D file length invalid");
    require(count <= (length-32)/8, "J3D chunk count invalid");
    std::size_t pos = 32;
    std::vector<std::string> chunks;
    for (std::uint32_t i = 0; i < count; ++i) {
        require(pos <= length && length-pos >= 8, "J3D chunk header truncated");
        range(model, pos, 8);
        const auto size = be32(model, pos+4);
        require(size >= 8 && size <= length-pos, "J3D chunk length invalid");
        chunks.emplace_back(reinterpret_cast<const char*>(model.data()+pos), 4);
        pos += size;
    }
    require(pos == length || ((pos+31) & ~std::size_t(31)) == length,
            "J3D chunks do not cover the declared file length");
    return chunks;
}
}
