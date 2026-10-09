#include "p2_font.h"
#include "JSystem/JUtility/JUTFont.h"
#include "JSystem/JKernel/JKRArchive.h"
#include <cstring>

namespace {
class ResidentFont : public JUTResFont {
    void* bytes;
public:
    ResidentFont(void* data,JKRHeap* heap):JUTResFont(static_cast<ResFONT*>(data),heap),bytes(data) {}
    ~ResidentFont() override {
        // JUTResFont's destructor releases only its pointer tables; it does not
        // dereference the backing resource after this derived destructor.
        JKRHeap::free(bytes,nullptr);
    }
};
}
JUTFont* p2_load_resident_font(const char* archivePath,const char* resourceName,JKRHeap* heap) {
    if(!archivePath||!resourceName) return nullptr;
    if(!heap) heap=JKRHeap::getCurrentHeap();
    if(!heap) return nullptr;
    JKRArchive* archive=JKRArchive::mount(archivePath,JKRArchive::EMM_Mem,heap,JKRArchive::EMD_Head);
    if(!archive) return nullptr;
    const void* resource=archive->getResource(resourceName);
    const u32 size=resource ? archive->getResSize(resource) : 0;
    void* copy=nullptr;
    if(size>=32 && std::memcmp(resource,"FONTbfn1",8)==0) {
        copy=JKRHeap::alloc(size,32,heap);
        if(copy) std::memcpy(copy,resource,size);
    }
    archive->unmount();
    if(!copy) return nullptr;
    auto* font=new(heap,0) ResidentFont(copy,heap);
    if(!font) { JKRHeap::free(copy,nullptr);return nullptr; }
    if(!font->isValid()) { delete font;return nullptr; }
    return font;
}
