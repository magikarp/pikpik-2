#include "p2_texture_refs.h"
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace {
struct TexturePointers { void* image; void* palette; };
// The table outlives game heaps and must not consume them; global new allocates from the current one.
template <typename T> struct HostAllocator {
    using value_type = T;
    HostAllocator() = default;
    template <typename U> HostAllocator(const HostAllocator<U>&) {}
    T* allocate(std::size_t n) {
        if (void* memory = std::malloc(n * sizeof(T))) return static_cast<T*>(memory);
        std::abort();
    }
    void deallocate(T* memory, std::size_t) { std::free(memory); }
    template <typename U> bool operator==(const HostAllocator<U>&) const { return true; }
    template <typename U> bool operator!=(const HostAllocator<U>&) const { return false; }
};
using CopyTable = std::unordered_map<const void*, TexturePointers, std::hash<const void*>, std::equal_to<const void*>,
                                     HostAllocator<std::pair<const void* const, TexturePointers>>>;
// Function-local construction avoids ordering against game global constructors.
struct State { std::mutex mutex; CopyTable copies; };
State& state() { static State value; return value; }
void* relative(const void* header, uint32_t offset) {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(header) + static_cast<int32_t>(offset));
}
TexturePointers resolve(State& state, const void* header, uint32_t image, uint32_t palette) {
    const auto found = state.copies.find(header);
    return found != state.copies.end() ? found->second : TexturePointers{relative(header, image), relative(header, palette)};
}
}
void* p2_texture_image(const void* header, uint32_t offset) {
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex);
    return resolve(s, header, offset, 0).image;
}
void* p2_texture_palette(const void* header, uint32_t offset) {
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex);
    return resolve(s, header, 0, offset).palette;
}
void p2_texture_copy(const void* destination, const void* source, uint32_t imageOffset, uint32_t paletteOffset) {
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex);
    const auto pointers = resolve(s, source, imageOffset, paletteOffset);
    s.copies[destination] = pointers;
}
void p2_texture_forget(const void* header) {
    auto& s = state(); std::lock_guard<std::mutex> lock(s.mutex);
    s.copies.erase(header);
}
