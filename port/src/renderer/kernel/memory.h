// SCDA-XBERecomp's allocator API (src/kernel/memory.h) on xboxrecomp's
// contiguous arena and guest heap.
#pragma once
#include <cstdint>

extern "C" {
uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
void xbox_HeapFree(uint32_t xbox_va);
}

namespace mem {

// MmAllocateContiguousMemory memory lives at 0x80000000 + its physical address
constexpr uint32_t kContigBase = 0x80000000u;
constexpr uint32_t kContigSize = 64u << 20;

inline uint32_t VirtualFromPhysical(uint32_t pa) { return pa < kContigSize ? kContigBase + pa : pa; }
inline uint32_t PhysicalFromVirtual(uint32_t va) {
    return va >= kContigBase && va - kContigBase < kContigSize ? va - kContigBase : va;
}
inline uint32_t AllocContiguous(uint32_t size, uint32_t, uint32_t, uint32_t align) {
    return xbox_ContiguousAlloc(size, align ? align : 0x1000);
}
inline uint32_t AllocSystem(uint32_t size, uint32_t align = 16) { return xbox_HeapAlloc(size, align); }
inline void FreeSystem(uint32_t va) { xbox_HeapFree(va); }

}  // namespace mem
