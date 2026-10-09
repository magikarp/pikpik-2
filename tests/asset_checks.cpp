#include "p2_assets.h"
#include <cstdio>
#include <functional>
#include <stdexcept>

static void require(bool value) { if (!value) throw std::runtime_error("asset regression failed"); }
static void rejects(const std::function<void()>& operation) {
    try { operation(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error("malformed asset accepted");
}
static void word(p2::Bytes& b, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) b.at(at+i) = value >> (24-8*i);
}
int main() {
    try {
        p2::Bytes compressed(16, 0);
        compressed[0]='Y'; compressed[1]='a'; compressed[2]='z'; compressed[3]='0';
        word(compressed, 4, 6);
        compressed.insert(compressed.end(), {0xe0, 'a', 'b', 'c', 0x10, 0x02});
        require(p2::decompressYaz0(compressed) == p2::Bytes({'a','b','c','a','b','c'}));
        compressed[19] = 'x'; // A repeated backreference reads the updated literal.
        require(p2::decompressYaz0(compressed) == p2::Bytes({'a','b','x','a','b','x'}));
        auto malformed = compressed; malformed.pop_back();
        rejects([&] { p2::decompressYaz0(malformed); });
        malformed = compressed; malformed[16] = 0;
        rejects([&] { p2::decompressYaz0(malformed); });
        malformed = compressed; word(malformed, 4, 5);
        rejects([&] { p2::decompressYaz0(malformed); });

        p2::Bytes yay(25, 0);
        yay[0]='Y'; yay[1]='a'; yay[2]='y'; yay[3]='0';
        word(yay, 4, 6); word(yay, 8, 20); word(yay, 12, 22);
        word(yay, 16, 0xe0000000); yay[20]=0x10; yay[21]=2;
        yay[22]='a'; yay[23]='b'; yay[24]='c';
        require(p2::decompressResource(yay) == p2::Bytes({'a','b','c','a','b','c'}));
        malformed=yay; word(malformed, 8, 16);
        rejects([&] { p2::decompressResource(malformed); });
        malformed=yay; word(malformed, 12, 21);
        rejects([&] { p2::decompressResource(malformed); });
        malformed=yay; malformed.pop_back();
        rejects([&] { p2::decompressResource(malformed); });
        malformed=yay; malformed[21]=3;
        rejects([&] { p2::decompressResource(malformed); });
        // One literal followed by an overlapping extended run of 18 bytes.
        yay.resize(24); word(yay, 4, 19); word(yay, 16, 0x80000000);
        yay[20]=0; yay[21]=0; yay[22]='x'; yay[23]=0;
        require(p2::decompressResource(yay) == p2::Bytes(19, 'x'));
        word(yay, 4, 10);
        require(p2::decompressResource(yay) == p2::Bytes(10, 'x'));
        word(yay, 4, 0xffffffff);
        rejects([&] { p2::decompressResource(yay); });
        require(p2::decompressResource(p2::Bytes({'r','a','w'})) == p2::Bytes({'r','a','w'}));

        p2::Bytes j3d(40, 0);
        j3d[0]='J'; j3d[1]='3'; j3d[2]='D'; j3d[3]='1';
        word(j3d, 8, 64); word(j3d, 12, 1);
        j3d[32]='A'; j3d[33]='N'; j3d[34]='F'; j3d[35]='1'; word(j3d, 36, 8);
        require(p2::readJ3dChunks(j3d) == std::vector<std::string>({"ANF1"}));
        word(j3d, 12, 2);
        rejects([&] { p2::readJ3dChunks(j3d); });
        word(j3d, 12, 1); word(j3d, 36, 128);
        rejects([&] { p2::readJ3dChunks(j3d); });
        rejects([] { p2::readRarc(p2::Bytes(64, 0)); });
        std::puts("Yaz0/Yay0 backreferences, bounded streams, malformed input and J3D alignment checks passed");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
