#include "p2_layout.h"
#include "p2_endian.h"
#include <cstdint>

bool p2_validate_2d_references(const void* data, std::size_t size) {
    if(!data || size<2) return false;
    const auto* b=static_cast<const unsigned char*>(data);
    const std::size_t count=p2_read_big<std::uint16_t>(b);
    if(count>(size-2)/2) return false;
    const std::size_t header=2+count*2;
    for(std::size_t i=0;i<count;++i) {
        const std::size_t at=p2_read_big<std::uint16_t>(b+2+i*2);
        if(at<header || at>size || size-at<2 || b[at+1]>size-at-2) return false;
    }
    return true;
}
