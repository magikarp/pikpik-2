#include "JSystem/JSupport/JSUStream.h"
#include "JSystem/J3D/J3DFileBlock.h"
#include "JSystem/J2D/J2DPane.h"
#include "p2_assets.h"
#include "p2_layout.h"
#include "JSystem/J2D/J2DManage.h"
#include <climits>
#include <cstdio>
#include <cstring>

static int failures;
static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
int main(int argc, char** argv) {
    static_assert(sizeof(J3DFileHeader) == 32, "J3D file header layout");
    static_assert(sizeof(J3DFileBlockBase) == 8, "J3D block header layout");
    static_assert(sizeof(J2DScrnBlockHeader) == 8, "J2D block header layout");
    static_assert(sizeof(J2DScreenInfoBlock) == 16, "J2D screen info layout");
    static_assert(sizeof(J2DTextBoxBlock) == 32, "J2D text box layout");
    const u8 bytes[] = {0xff,0xfe,0x12,0x34,0x89,0xab,0xcd,0xef,0,3,'p','i','k'};
    JSUMemoryInputStream stream(bytes, sizeof(bytes));
    check(stream.readS16() == -2, "signed big-endian short");
    s16 value;
    check(stream.read(value) == 2 && value == 0x1234, "typed reference read");
    check(stream.readU32() == 0x89abcdefu, "big-endian word");
    char text[4] = {};
    check(stream.read(text) == text && !std::strcmp(text, "pik"), "big-endian string length");
    check(stream.getAvailable() == 0, "string consumes exact input");
    check(stream.readU16() == 0 && (stream.mIsEOFMaybe & 1), "truncated scalar flags EOF deterministically");
    stream.seek(0, SEEK_SET);
    u8 raw[4] = {};
    check(stream.peek(raw, 4) == 4 && !std::memcmp(raw, bytes, 4) && stream.getPosition() == 0,
          "raw peek preserves bytes and position");
    stream.seek(4, SEEK_SET);
    stream.seek(INT_MAX, SEEK_CUR);
    check(stream.getAvailable() == 0, "seek overflow clamps to end");
    stream.seek(INT_MIN, SEEK_CUR);
    check(stream.getPosition() == 0, "large negative seek clamps to start");
    check(stream.readData(raw, -1) == 0 && stream.getPosition() == 0, "negative reads do not move position");
    stream.seek(2, SEEK_SET);
    u8 remainder[sizeof(bytes)] = {};
    check(stream.readData(remainder, INT_MAX) == sizeof(bytes)-2, "oversized read clamps without overflow");
    const u8 floatBytes[] = {0x3f,0xc0,0,0};
    float encoded;
    std::memcpy(&encoded, floatBytes, 4);
    check(p2_big_endian(encoded) == 1.5f, "float conversion preserves bit representation");
    unsigned char references[206]={0,1,0,4,3,200};
    std::memset(references+6,'a',200);
    check(p2_validate_2d_references(references,sizeof(references)),"long resource reference validates");
    auto* refs=reinterpret_cast<J2DResReference*>(references);
    check(refs->mCount==1 && std::strlen(refs->getName(0))==200,"original reference lookup decodes count, offset and unsigned name length");
    check(!refs->getResReference(1) && !refs->getResReference(0xffff),"reference index bounds");
    check(!p2_validate_2d_references(references,sizeof(references)-1),"truncated reference name rejected");
    references[3]=2;
    check(!p2_validate_2d_references(references,sizeof(references)),"reference into offset table rejected");
    references[3]=255;
    check(!p2_validate_2d_references(references,sizeof(references)),"reference beyond table rejected");
    if (argc == 2) {
        const auto archive = p2::decompressYaz0(p2::readAsset(argv[1], "new_screen/eng/title.szs"));
        int layouts = 0, panes = 0, resourceNames=0;
        for (const auto& entry : p2::readRarc(archive)) {
            const auto begin = archive.begin()+entry.offset;
            const auto data = p2::decompressYaz0(p2::Bytes(begin, begin+entry.size));
            if (data.size() < 32 || std::memcmp(data.data(), "SCRN", 4)) continue;
            JSUMemoryInputStream layout(data.data(), data.size());
            check(layout.readU32() == 0x5343524e, "real title SCRN magic");
            const auto format = layout.readU32();
            check(format == 0x626c6f31 || format == 0x626c6f32, "real title layout version");
            const auto length = layout.readU32();
            const auto blocks = layout.readU32();
            check(length <= data.size() && length >= 32 && blocks > 0, "real title layout header");
            layout.seek(0, SEEK_SET);
            J3DFileHeader header;
            layout.read(&header, sizeof(header));
            check(header.mJ3dVersion == 0x5343524e && header.mFileVersion == format && header.mBlockCount == blocks,
                  "original J2D signature check sees host-order fields after raw read");
            J2DScreenInfoBlock info;
            layout.read(&info, sizeof(info));
            check(info.mBloBlockType == 0x494e4631 && info.mWidth > 0 && info.mWidth <= 1024 &&
                  info.mHeight > 0 && info.mHeight <= 1024, "real title screen info dimensions");
            layout.seek(32, SEEK_SET);
            for (u32 i = 0; i < blocks; ++i) {
                const int start = layout.getPosition();
                layout.readU32();
                const auto size = layout.readU32();
                if (size < 8 || size > length || start > length-size) {
                    check(false, "real title block bounds"); break;
                }
                layout.seek(start, SEEK_SET);
                J2DScrnBlockHeader diskBlock;
                layout.read(&diskBlock, sizeof(diskBlock));
                check(diskBlock.mBlockLength == size, "raw J2D block header conversion");
                if (diskBlock.mBloBlockType == 0x50414e32) {
                    ++panes;
                    check(size >= sizeof(J2DPaneExBlock), "PAN2 contains full pane record");
                    layout.seek(start, SEEK_SET);
                    J2DPaneExBlock pane;
                    layout.read(&pane, sizeof(pane));
                    check(isfinite((float)pane.mWidth) && isfinite((float)pane.mHeight) &&
                          isfinite((float)pane.mWidthScale), "PAN2 native float fields");
                }
                if (diskBlock.mBloBlockType==0x54455831 || diskBlock.mBloBlockType==0x464e5431) {
                    layout.seek(start+12,SEEK_SET);
                    const u32 offset=layout.readU32();
                    check(offset>=16 && offset<=size,"reference block payload offset");
                    if(offset>=16 && offset<=size) {
                        const auto* raw=data.data()+start+offset;
                        const size_t available=size-offset;
                        const bool valid=p2_validate_2d_references(raw,available);
                        check(valid,"retail title texture/font reference bounds");
                        if(valid) {
                            auto* refs=reinterpret_cast<const J2DResReference*>(raw);
                            for(u16 idx=0;idx<refs->mCount;++idx) {
                                const auto* item=reinterpret_cast<const unsigned char*>(refs->getResReference(idx));
                                check(item>=raw && item+2+item[1]<=raw+available,"original reference pointer stays in block");
                                if(item[0]==2 || item[0]==3) {
                                    const char* name=refs->getName(idx);
                                    check(std::strlen(name)==item[1] && !std::memcmp(name,item+2,item[1]),"original title resource name matches disk bytes");
                                }
                                ++resourceNames;
                            }
                        }
                    }
                }
                layout.seek(start+size, SEEK_SET);
            }
            std::printf("  %s: %u blocks, end %d, declared %u, bytes %zu\n",
                        entry.path.c_str(), blocks, layout.getPosition(), length, data.size());
            // Retail BLO files include up to 31 bytes of final DVD alignment.
            check(layout.getPosition() == length || ((layout.getPosition()+31u) & ~31u) == length,
                  "real title block walk reaches declared end or its 32-byte padding");
            ++layouts;
        }
        check(layouts == 7, "all seven real title layouts parsed through JSystem streams");
        check(resourceNames>0,"title resource reference coverage");
        std::printf("Validated %d title texture/font references\n",resourceNames);
        check(panes > 0, "real title data exercises native PAN2 records");
        std::printf("JSystem stream loaded %d title layouts and %d PAN2 records\n", layouts, panes);
    }
    return failures ? 1 : 0;
}
