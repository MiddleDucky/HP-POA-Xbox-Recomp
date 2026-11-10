// SCDA-XBERecomp's guest memory API (src/kernel/kernel.h), on xboxrecomp's
// flat Xbox window. Only what the ported renderer and D3D8 layer use.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

#include "kernel/memory.h"

extern "C" ptrdiff_t g_xbox_mem_offset;

namespace kernel {

inline uint8_t* Base() { return reinterpret_cast<uint8_t*>(g_xbox_mem_offset); }
inline void* HostPtr(uint32_t va) { return Base() + va; }
template <typename T>
inline T& G(uint32_t va) {
    return *reinterpret_cast<T*>(Base() + va);
}
inline uint32_t R32(uint32_t va) {
    uint32_t v;
    std::memcpy(&v, Base() + va, 4);
    return v;
}
inline void W32(uint32_t va, uint32_t v) { std::memcpy(Base() + va, &v, 4); }
inline uint16_t R16(uint32_t va) {
    uint16_t v;
    std::memcpy(&v, Base() + va, 2);
    return v;
}
inline void W16(uint32_t va, uint16_t v) { std::memcpy(Base() + va, &v, 2); }
inline uint8_t R8(uint32_t va) { return Base()[va]; }

}  // namespace kernel
