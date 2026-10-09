#include "p2_host_compat.h"
#include "Dolphin/dvd.h"
#include "p2_assets.h"
#include "p2_dvd.h"
#include "p2_game_alloc.h"
#include <algorithm>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {
struct Entry { std::string name, path; bool directory; u32 offset, length, next; };
struct Disc {
    std::mutex mutex;
    std::filesystem::path root;
    p2::Bytes fst;
    DVDDiskID id{};
    std::vector<Entry> entries;
    std::unordered_map<std::string, s32> names;
    std::unordered_map<const DVDFileInfo*, s32> handles;
    std::string current;
};
Disc& disc() { static Disc value; return value; }
std::string folded(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a'-'A';
    return value;
}
s32 lookup(Disc& d, const char* path) {
    if (!path) return -1;
    // SDK DVDConvertPathToEntrynum restarts at the root whenever a component
    // begins with '/', so "a/b//c/d" names "/c/d". Pellet text archives rely
    // on this: their loader prefixes a directory onto an absolute path.
    std::string text(path);
    if (const auto restart = text.rfind("//"); restart != std::string::npos) text.erase(0, restart+1);
    std::filesystem::path p(text);
    if (p.is_absolute()) p = p.relative_path();
    else p = std::filesystem::path(d.current) / p;
    p = p.lexically_normal();
    if (!p.empty() && *p.begin() == "..") return -1;
    auto key = p.generic_string();
    if (key == ".") key.clear();
    while (!key.empty() && key.back() == '/') key.pop_back();
    const auto it = d.names.find(folded(key));
    return it == d.names.end() ? -1 : it->second;
}
bool openEntry(Disc& d, s32 index, DVDFileInfo* info) {
    if (!info || index < 0 || static_cast<size_t>(index) >= d.entries.size() || d.entries[index].directory) return false;
    const auto& entry = d.entries[index];
    *info = DVDFileInfo{};
    info->startAddr = entry.offset;
    info->length = entry.length;
    info->cBlock.state = DVD_STATE_END;
    d.handles[info] = index;
    return true;
}
}

bool p2_dvd_mount(const char* extractedDiscRoot) {
    auto& d = disc(); std::lock_guard<std::mutex> lock(d.mutex);
    if (!d.handles.empty()) return false;
    try {
        auto root = std::filesystem::canonical(extractedDiscRoot);
        auto boot = p2::readAsset(root, "sys/boot.bin");
        if (boot.size() < sizeof(DVDDiskID) || std::memcmp(boot.data(), "GPVE01", 6)) throw std::runtime_error("DVD mount requires GPVE01");
        auto fst = p2::readAsset(root, "sys/fst.bin");
        auto be = [&](size_t at) -> u32 {
            if (at > fst.size() || fst.size()-at < 4) throw std::runtime_error("Truncated FST");
            return (u32(fst[at])<<24) | (u32(fst[at+1])<<16) | (u32(fst[at+2])<<8) | fst[at+3];
        };
        const auto count = be(8);
        if (!count || size_t(count)*12 > fst.size() || be(0)>>24 != 1) throw std::runtime_error("Invalid FST root");
        const size_t strings = size_t(count)*12;
        std::vector<Entry> entries(count);
        std::unordered_map<std::string, s32> names;
        std::vector<u32> parents{0};
        entries[0] = {"", "", true, 0, 0, count};
        names[""] = 0;
        for (u32 i = 1; i < count; ++i) {
            while (!parents.empty() && i >= entries[parents.back()].next) parents.pop_back();
            if (parents.empty()) throw std::runtime_error("Invalid FST hierarchy");
            const auto parent = parents.back();
            const auto flags = be(size_t(i)*12);
            const size_t offset = strings + (flags & 0xffffff);
            if (offset >= fst.size()) throw std::runtime_error("Invalid FST name offset");
            const auto* start = reinterpret_cast<const char*>(fst.data()+offset);
            const auto* end = static_cast<const char*>(std::memchr(start, 0, fst.size()-offset));
            if (!end) throw std::runtime_error("Unterminated FST name");
            std::string name(start, end);
            if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos) throw std::runtime_error("Invalid FST path component");
            auto& e = entries[i];
            e = {name, entries[parent].path.empty() ? name : entries[parent].path+"/"+name,
                 bool(flags>>24), be(size_t(i)*12+4), be(size_t(i)*12+8), i+1};
            if (e.directory) {
                e.next = e.length;
                if (e.offset != parent || e.next <= i || e.next > entries[parent].next) throw std::runtime_error("Invalid FST directory range");
                parents.push_back(i);
            }
            if (!names.emplace(folded(e.path), i).second) throw std::runtime_error("Duplicate FST path");
        }
        d.root = root/"files"; d.fst = std::move(fst); d.entries = std::move(entries); d.names = std::move(names);
        d.current.clear(); std::memcpy(&d.id, boot.data(), sizeof(d.id));
        return true;
    } catch (const std::exception& error) { std::fprintf(stderr, "DVD mount: %s\n", error.what()); return false; }
}

// Entry points that touch the host tables allocate from host memory: they are
// called from game threads whose global new targets the current JKR heap.
extern "C" {
void DVDInit() { if (disc().entries.empty()) p2_dvd_mount(P2_DISC_ROOT); }
BOOL DVDOpen(char* path, DVDFileInfo* info) {
    P2HostScratchScope hostStorage;
    auto& d = disc(); std::lock_guard<std::mutex> lock(d.mutex); return openEntry(d, lookup(d,path), info);
}
BOOL DVDFastOpen(s32 index, DVDFileInfo* info) {
    P2HostScratchScope hostStorage;
    auto& d = disc(); std::lock_guard<std::mutex> lock(d.mutex); return openEntry(d,index,info);
}
BOOL DVDClose(DVDFileInfo* info) {
    P2HostScratchScope hostStorage;
    auto& d = disc(); std::lock_guard<std::mutex> lock(d.mutex); return d.handles.erase(info) ? TRUE : FALSE;
}
s32 DVDConvertPathToEntrynum(char* path) {
    P2HostScratchScope hostStorage;
    auto& d = disc(); std::lock_guard<std::mutex> lock(d.mutex); return lookup(d,path);
}
s32 DVDReadPrio(DVDFileInfo* info, void* output, s32 length, s32 offset, s32) {
    P2HostScratchScope hostStorage;
    auto& d = disc(); std::lock_guard<std::mutex> lock(d.mutex);
    const auto found = d.handles.find(info);
    if (found == d.handles.end() || length < 0 || offset < 0 || (!output && length)) return DVD_RESULT_FATAL_ERROR;
    const auto& e = d.entries[found->second];
    const uint64_t padded = (uint64_t(e.length)+31) & ~uint64_t(31);
    if (uint64_t(offset)+uint64_t(length) > padded) return DVD_RESULT_FATAL_ERROR;
    try {
        // Canonicalization prevents an extracted-file symlink escaping the disc.
        const auto root = std::filesystem::canonical(d.root);
        const auto path = std::filesystem::canonical(root/e.path);
        const auto relative = path.lexically_relative(root);
        if (relative.empty() || *relative.begin() == ".." || std::filesystem::file_size(path) != e.length) return DVD_RESULT_FATAL_ERROR;
        std::ifstream input(path, std::ios::binary);
        const size_t actual = offset >= e.length ? 0 : std::min<u32>(length, e.length-offset);
        input.seekg(offset);
        if (actual && !input.read(static_cast<char*>(output), actual)) return DVD_RESULT_FATAL_ERROR;
        if (size_t(length) > actual) std::memset(static_cast<char*>(output)+actual, 0, size_t(length)-actual);
        info->cBlock.transferredSize = length; info->cBlock.state = DVD_STATE_END;
        return length;
    } catch (const std::exception&) { return DVD_RESULT_FATAL_ERROR; }
}
BOOL DVDReadAsyncPrio(DVDFileInfo* info,void* output,s32 length,s32 offset,DVDCallback callback,s32 priority) {
    P2HostScratchScope hostStorage;
    if(!info) return FALSE;
    // Completion is synchronous during bring-up. Invoke after DVDReadPrio has
    // released the disc lock: callbacks may query or close their own file.
    info->callback=callback;
    const s32 result=DVDReadPrio(info,output,length,offset,priority);
    if(result<0) info->cBlock.state=DVD_STATE_FATAL_ERROR;
    if(callback) callback(result,info);
    return TRUE;
}
s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) { return block ? block->state : DVD_STATE_FATAL_ERROR; }
s32 DVDGetDriveStatus() { return disc().entries.empty() ? DVD_STATE_NO_DISK : DVD_STATE_END; }
s32 DVDGetTransferredSize(DVDFileInfo* info) { return info ? info->cBlock.transferredSize : 0; }
DVDDiskID* DVDGetCurrentDiskID() { return disc().entries.empty() ? nullptr : &disc().id; }
BOOL DVDCheckDisk() { return !disc().entries.empty(); }
BOOL DVDChangeDir(char* path) {
    P2HostScratchScope hostStorage;
    auto& d = disc(); std::lock_guard<std::mutex> lock(d.mutex);
    const auto i = lookup(d,path); if (i < 0 || !d.entries[i].directory) return FALSE;
    d.current = d.entries[i].path; return TRUE;
}
BOOL DVDGetCurrentDir(char* output, u32 length) {
    P2HostScratchScope hostStorage;
    auto& d = disc(); std::lock_guard<std::mutex> lock(d.mutex);
    const auto path = "/"+d.current;
    if (!output || path.size()+1 > length) return FALSE;
    std::memcpy(output, path.c_str(), path.size()+1); return TRUE;
}
BOOL DVDOpenDir(char* path, DVDDir* output) {
    P2HostScratchScope hostStorage;
    auto& d = disc(); std::lock_guard<std::mutex> lock(d.mutex);
    const auto i = lookup(d,path); if (!output || i < 0 || !d.entries[i].directory) return FALSE;
    *output = {u32(i), u32(i+1), d.entries[i].next}; return TRUE;
}
BOOL DVDReadDir(DVDDir* directory, DVDDirEntry* output) {
    P2HostScratchScope hostStorage;
    auto& d = disc(); std::lock_guard<std::mutex> lock(d.mutex);
    if (!directory || !output || directory->location >= directory->next || directory->next > d.entries.size()) return FALSE;
    const auto i = directory->location; auto& e = d.entries[i];
    *output = {i, e.directory, const_cast<char*>(e.name.c_str())}; directory->location = e.next; return TRUE;
}
BOOL DVDCloseDir(DVDDir* directory) { if (!directory) return FALSE; *directory = {}; return TRUE; }
}
