#ifndef SRC_DAWN_NATIVE_DEKO3D_IMAGEALLOCATIONDEKO3D_H_
#define SRC_DAWN_NATIVE_DEKO3D_IMAGEALLOCATIONDEKO3D_H_

#include <deko3d.h>

#include <cstdint>

namespace dawn::native::deko3d {

// A texture's memory: a range of a shared image chunk, or a dedicated block for large images.
struct ImageAllocation {
    DkMemBlock block = nullptr;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool dedicated = false;
};

}  // namespace dawn::native::deko3d

#endif  // SRC_DAWN_NATIVE_DEKO3D_IMAGEALLOCATIONDEKO3D_H_
