/*
 * Heap slack for vendor allocations.
 *
 * The vendor code was developed against glibc, whose malloc chunks carry spare
 * bytes, so small overruns go unnoticed there. macOS's allocator packs blocks
 * tightly and detects the damage later (a SIGTRAP inside malloc). One such
 * overrun is in Device_GetByteStream_IFACalData, which packs 24-bit values with
 * 4-byte stores advancing 3 bytes at a time, writing one byte past its 3*N-byte
 * output buffer on every Device_Open that loads IF calibration.
 *
 * Every heap request made by the vendor objects is therefore enlarged by a few
 * bytes. The returned pointers are ordinary heap pointers, so free(), delete
 * and realloc() by any code (vendor, libstdc++ or libSystem) are unaffected.
 */

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <new>

extern "C" {
#include "shim.h"
}

namespace {

constexpr size_t kSlack = 16;

size_t padded(size_t n) { return n > SIZE_MAX - kSlack ? n : n + kSlack; }

extern "C" void *a_malloc(size_t n) { return std::malloc(padded(n)); }

extern "C" void *a_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return nullptr;
    }
    return std::calloc(1, padded(count * size));
}

extern "C" void *a_realloc(void *p, size_t n) { return std::realloc(p, padded(n)); }

/* Linux ENOMEM/EINVAL values match macOS, and posix_memalign returns (not sets) them. */
extern "C" int a_posix_memalign(void **out, size_t align, size_t n) {
    return posix_memalign(out, align, padded(n));
}

void *a_new(size_t n) { return ::operator new(padded(n)); }
void *a_new_array(size_t n) { return ::operator new[](padded(n)); }

}  // namespace

extern "C" const struct shim_sym shim_alloc_syms[] = {
    {"malloc", (void *)a_malloc},
    {"calloc", (void *)a_calloc},
    {"realloc", (void *)a_realloc},
    {"posix_memalign", (void *)a_posix_memalign},
    {"_Znwm", (void *)a_new},
    {"_Znam", (void *)a_new_array},
    {nullptr, nullptr},
};
