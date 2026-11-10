/* MinGW build only. The 32-bit Windows Dawn package is built with MSVC and
 * used through its C interface, with one exception: Aurora constructs a
 * dawn::native::DawnInstanceDescriptor, whose constructor is a C++ function
 * exported under MSVC's name for it. The constructor only tags the chained
 * structure, so it is defined here for this compiler. */
#include <dawn/native/DawnNative.h>

namespace dawn::native {
DawnInstanceDescriptor::DawnInstanceDescriptor() { sType = wgpu::SType::DawnInstanceDescriptor; }
} // namespace dawn::native
