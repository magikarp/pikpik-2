#include "JSystem/JUtility/JUTFont.h"
#include "JSystem/JUtility/JUTResFONT_Ascfont_fix12.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "p2_memory.h"
#include "p2_font.h"
#include "p2_dvd.h"
#include "p2_assets.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static void require(bool ok,const char* what) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); } }
static u16 be16(const u8* p) { return (u16(p[0])<<8)|p[1]; }
static u32 be32(const u8* p) { return (u32(be16(p))<<16)|be16(p+2); }
// Test-only GX recorder: verify the original glyph selection's output without
// claiming rendering. The production source retains its real GX calls.
static struct { GXTexObj* object; void* image; u16 width,height; GXTexFmt format; unsigned loads; } texture;
extern "C" void GXInitTexObj(GXTexObj* obj,void* image,u16 width,u16 height,GXTexFmt format,GXTexWrapMode s,GXTexWrapMode t,GXBool mip) {
    require(s==GX_CLAMP && t==GX_CLAMP && !mip,"font texture wrapping");
    texture.object=obj; texture.image=image; texture.width=width; texture.height=height; texture.format=format;
}
extern "C" void GXInitTexObjLOD(GXTexObj* obj,GXTexFilter min,GXTexFilter mag,f32 low,f32 high,f32 bias,GXBool clamp,GXBool edge,GXAnisotropy aniso) {
    require(obj==texture.object && min==GX_LINEAR && mag==GX_LINEAR && low==0 && high==0 && bias==0 && !clamp && !edge && aniso==GX_ANISO_1,"font texture filtering");
}
extern "C" void GXLoadTexObj(GXTexObj* obj,GXTexMapID map) {
    require(obj==texture.object && map==GX_TEXMAP0,"font texture binding"); ++texture.loads;
}
int main(int argc,const char** argv) {
    require(argc==2 && p2_memory_init(128*1024*1024),"arena and disc");
    auto* root=JKRExpHeap::createRoot(16,false);
    require(root && p2_dvd_mount(argv[1]),"heap and DVD");
    {
        const auto* embedded=JUTResFONT_Ascfont_fix12;
        require(!std::memcmp(embedded,"FONTbfn1",8) && be32(embedded+8)==0x4160 && be32(embedded+12)==4,"embedded startup font byte order");
        size_t offset=32;
        for(unsigned i=0;i<4;++i) {
            require(offset+8<=0x4160,"embedded block header extent");
            const auto size=be32(embedded+offset+4);
            require(size>=8 && size<=0x4160-offset,"embedded block extent"); offset+=size;
        }
        require(offset==0x4160,"embedded block chain");
        JUTResFont startup(reinterpret_cast<const ResFONT*>(embedded),root);
        require(startup.isValid() && startup.mWidthBlockCount==1 && startup.mGlyphBlockCount==1 && startup.mMapBlockCount==1,"original startup font constructor");
        require(startup.getAscent()==12 && startup.getDescent()==0,"embedded font metrics");
        for(int chr=32;chr<127;++chr) {
            const auto code=startup.getFontCode(chr);
            require(code==chr-32,"embedded ASCII character mapping");
            const auto loads=texture.loads; startup.loadImage(code,GX_TEXMAP0);
            require(texture.loads==loads+1 && texture.width==512 && texture.height==64,"embedded ASCII glyph selection");
        }
    }
    size_t checks=0,fonts=0;
    for(const char* path:{"message/font_foreign.szs","message/font_jpn.szs"}) {
        auto bytes=p2::decompressResource(p2::readAsset(std::filesystem::path(argv[1])/"files",path));
        auto resources=p2::readRarc(bytes);
        auto* archive=JKRArchive::mount(path,JKRArchive::EMM_Mem,root,JKRArchive::EMD_Head);
        require(archive,"font archive");
        for(const auto& entry:resources) {
            if(entry.size<32 || std::memcmp(bytes.data()+entry.offset,"FONTbfn1",8)) continue;
            auto* raw=static_cast<const u8*>(archive->getResource(("/"+entry.path).c_str()));
            require(raw && archive->getResSize(raw)==entry.size,"font resource extent");
            require(be32(raw+8)==entry.size,"declared file size");
            const u8* info=nullptr; size_t cursor=32;
            for(u32 block=0;block<be32(raw+12);++block) {
                require(cursor+8<=entry.size,"block header extent");
                const u32 size=be32(raw+cursor+4);
                require(size>=8 && size<=entry.size-cursor,"block payload extent");
                if(!std::memcmp(raw+cursor,"INF1",4)) info=raw+cursor;
                cursor+=size;
            }
            require(info && be16(info+8)<3,"font encoding");
            const auto freeBefore=root->getTotalFreeSize();
            {
                JUTResFont font(reinterpret_cast<const ResFONT*>(raw),root);
                require(font.isValid(),"original font constructor");
                require(font.getAscent()==be16(info+10) && font.getDescent()==be16(info+12) &&
                    font.getWidth()==be16(info+14) && font.getLeading()==be16(info+16),"font metrics match disc values");
                for(int g=0;g<font.mGlyphBlockCount;++g) {
                    auto* glyph=reinterpret_cast<const u8*>(font.mGlyphBlocks[g]);
                    const unsigned rows=be16(glyph+22),cols=be16(glyph+24),sheet=be32(glyph+16);
                    require(rows && cols && sheet,"nonempty glyph sheet");
                    for(unsigned code=be16(glyph+8);code<=be16(glyph+10);++code) {
                        const unsigned local=code-be16(glyph+8),page=local/(rows*cols),cell=local%(rows*cols);
                        require(32+size_t(page+1)*sheet<=be32(glyph+4),"glyph sheet extent");
                        const auto loads=texture.loads;
                        font.loadImage(code,GX_TEXMAP0);
                        require(texture.loads==loads+1 && texture.image==glyph+32+page*sheet &&
                            texture.width==be16(glyph+26) && texture.height==be16(glyph+28) && texture.format==be16(glyph+20),"original glyph texture page and format");
                        require(font.mWidth==int(cell%rows*be16(glyph+12)) && font.mHeight==int(cell/rows*be16(glyph+14)),"original glyph cell coordinates");
                    }
                }
                for(int chr=0;chr<65536;++chr) {
                    int expected=be16(info+18),mapped=chr;
                    // SJIS fonts remap printable ASCII via an original fixed table;
                    // all remaining code points have a direct disk-only oracle.
                    if(font.getFontType()==2 && font.mMaxCode>=0x8000 && chr>=32 && chr<127) continue;
                    for(int m=0;m<font.mMapBlockCount;++m) {
                        auto* map=reinterpret_cast<const u8*>(font.mMapBlocks[m]);
                        if(mapped<be16(map+10) || mapped>be16(map+12)) continue;
                        const auto method=be16(map+8);
                        if(method==0) expected=mapped-be16(map+10);
                        else if(method==2) expected=be16(map+16+2*(mapped-be16(map+10)));
                        else if(method==3) {
                            for(unsigned j=0;j<be16(map+14);++j)
                                if(be16(map+16+4*j)==mapped) { expected=be16(map+18+4*j); break; }
                        } else if(method==1) {
                            int low=(mapped&255)-64; if(low>=64) --low;
                            expected=low+((mapped>>8)-0x88)*0xbc - 0x5e + (be16(map+14)==1 ? be16(map+16) : 0x31c);
                        } else require(false,"known mapping method");
                        break;
                    }
                    require(font.getFontCode(chr)==expected,"original character lookup matches console map");
                    JUTFont::TWidth width; font.getWidthEntry(chr,&width);
                    u8 left=0,advance=be16(info+14);
                    for(int w=0;w<font.mWidthBlockCount;++w) {
                        auto* block=reinterpret_cast<const u8*>(font.mWidthBlocks[w]);
                        if(expected>=be16(block+8) && expected<=be16(block+10)) {
                            left=block[12+2*(expected-be16(block+8))]; advance=block[13+2*(expected-be16(block+8))]; break;
                        }
                    }
                    require(width.w0==left && width.w1==advance,"character width matches disc table"); ++checks;
                }
            }
            require(root->getTotalFreeSize()==freeBefore,"font metadata teardown"); ++fonts;
        }
        archive->unmount();
    }
    for(const char* path:{"message/font_foreign.szs","message/font_jpn.szs"}) {
        for(unsigned cycle=0;cycle<3;++cycle) {
            const auto before=root->getTotalFreeSize();
            JUTFont* owned=p2_load_resident_font(path,"pikmin2main.bfn",root);
            require(owned && owned->isValid(),"resident font after source archive unmount");
            auto* resource=static_cast<JUTResFont*>(owned);
            for(int character:{65,97,0x8140,0x82a0}) {
                JUTFont::TWidth width;owned->getWidthEntry(character,&width);
                const int code=resource->getFontCode(character);
                resource->loadImage(code,GX_TEXMAP0);
                require(texture.image && texture.width && texture.height,"owned glyph sheet survives archive teardown");
            }
            delete owned;
            require(root->getTotalFreeSize()==before,"resident font releases object, tables and backing storage");
        }
        const auto before=root->getTotalFreeSize();
        require(!p2_load_resident_font(path,"missing-font.bfn",root),"missing font reports failure");
        require(root->getTotalFreeSize()==before,"missing resource cleans up archive");
    }
    require(fonts>=2,"both supplied font archives checked");
    std::printf("Original font CPU lookup: %zu fonts, %zu character mappings and widths; drawing untested.\n",fonts,checks);
}
