#include "p2_assets.h"
#include "p2_message_resource.h"
#include "JSystem/JMessage/TParse.h"
#include "JSystem/JMessage/TProcessor.h"
#include "JSystem/JMessage/TReference.h"
#include <cstdio>
#include <cstdlib>
static void require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
int main(int argc, const char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    require(argc == 2, "disc path");
    // Exercise both MID1 search paths and all split-ID encodings independently
    // of whichever forms the supplied language files happen to use.
    for (unsigned supplement = 0; supplement <= 4; ++supplement) {
        for (bool ordered : {false, true}) {
            unsigned char block[28] = {};
            block[9] = 3; block[10] = ordered ? 0x10 : 0; block[11] = supplement;
            const u32 values[] = {0x12345678, 0x23456789, 0x3456789a};
            for (unsigned i = 0; i < 3; ++i) {
                const auto value = p2_big_endian(values[ordered ? i : 2-i]);
                std::memcpy(block + 16 + 4*i, &value, 4);
            }
            JMessage::TResource res;
            res.setData_block_messageID(block);
            for (unsigned i = 0; i < 3; ++i) {
                const u32 value = values[ordered ? i : 2-i];
                const unsigned shift = supplement*8;
                const u32 low = shift == 32 ? 0 : value >> shift;
                const u32 high = shift == 32 ? value : value & (shift ? ((u32(1)<<shift)-1) : 0);
                bool valid = false;
                require(res.toMessageIndex_messageID(low, high, &valid) == i && valid, "all MID1 forms and search orders");
            }
            require(res.toMessageIndex_messageID(0, 0, nullptr) == 0xffff, "missing message ID");
        }
    }
    unsigned files = 0, entries = 0;
    for (const char* language : {"eng", "jpn", "fra", "spa", "hol", "ger", "ita"}) {
        auto bytes = p2::decompressResource(p2::readAsset(argv[1], std::string("message/mesRes_") + language + ".szs"));
        for (const auto& entry : p2::readRarc(bytes)) {
            const auto* raw = bytes.data() + entry.offset;
            if (entry.size < 8 || (std::memcmp(raw,"MESGbmg1",8) && std::memcmp(raw,"MGCLbmc1",8))) continue;
            std::printf("%s: %s (%zu bytes)\n", language, entry.path.c_str(), entry.size);
            const bool color = !std::memcmp(raw,"MGCL",4);
            require(p2_validate_message_resource(raw, entry.size, color), "bounded real resource validation");
            for (std::size_t cut : {std::size_t(0), std::size_t(16), entry.size/2, entry.size-1})
                require(!p2_validate_message_resource(raw, cut, color), "truncated resource rejected");
            auto damaged = p2::Bytes(raw, raw + entry.size);
            damaged[36] = 0xff;
            require(!p2_validate_message_resource(damaged.data(), damaged.size(), color), "oversized block rejected");
            JMessage::TResourceContainer container;
            if (color) {
                JMessage::TParse_color parser(&container);
                require(parser.parse(raw, 0x20), "original color parser");
                require(container.mColor.mBlock.getRaw() == raw + 32, "original color block location");
            } else {
                JMessage::TParse parser(&container);
                require(parser.parse(raw, 0), "original message parser");
                auto* res = parser.mResource;
                JMessage::TReference reference;
                reference.setResourceContainer(&container);
                JMessage::TRenderingProcessor processor(&reference);

                require(res && res->mHeader.getSize() == entry.size && res->getMessageEntryNumber(), "original message metadata");
                for (unsigned i = 0; i < res->getMessageEntryNumber(); ++i) {
                    const auto* diskEntry = res->mInfo.getContent() + i * res->getMessageEntrySize();
                    const auto* text = res->getMessageText_messageIndex(i);
                    require(text == res->mMessages + p2_read_big<u32>(diskEntry), "original text offset lookup");
                    require(text >= reinterpret_cast<const char*>(raw) && text < reinterpret_cast<const char*>(raw + entry.size), "text inside file");
                    if (res->mMessageID.getRaw()) {
                        const u32 packed = p2_read_big<u32>(res->mMessageID.getContent() + 4*i);
                        const unsigned shift = res->mMessageID.get_formSupplement()*8;
                        const u32 low = shift == 32 ? 0 : packed >> shift;
                        const u32 high = shift == 32 ? packed : packed & (shift ? ((u32(1)<<shift)-1) : 0);
                        bool valid = false;
                        require(res->toMessageIndex_messageID(low, high, &valid) == i && valid, "original message ID lookup");
                        processor.setBegin_messageID(low, high, &valid);
                        require(valid && processor.mCurrent == text && processor.mResourceCache == res,
                                "original processor resolves message ID to text");
                    }
                    processor.setBegin_messageCode(res->getGroupID(), i);
                    require(processor.mResourceCache == res && processor.mCurrent == text,
                            "original processor resolves group/index to text");
                    ++entries;
                }
            }
            container.destroyResource_all(); ++files;
        }
    }
    require(files == 14, "all seven languages and color files");
    std::printf("Original JMessage parses %u files and resolves %u entries; malformed extents rejected.\n", files, entries);
}
