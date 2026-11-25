// Tests that compile shims/d3d12_xs_device.cpp without the XMem allocator (shims/xmem.cpp): the device proxy commits
// XMem views before it reads title memory (Leonida CE). There is no XMem here, so there is nothing to commit.
#include <cstddef>

namespace rt::xmem {
bool commit_physical_range(const void*, std::size_t) { return false; }
}  // namespace rt::xmem
