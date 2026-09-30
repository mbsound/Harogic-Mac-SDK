/*
 * Shared declarations for the glibc -> macOS compatibility layer.
 */
#ifndef SHIM_H
#define SHIM_H

#include <stddef.h>
#include <stdint.h>

/* glibc aarch64 va_list layout (AAPCS64 §B.4). */
typedef struct {
    void *stack;
    void *gr_top;
    void *vr_top;
    int gr_offs;
    int vr_offs;
} lx_va_list;

struct shim_sym {
    const char *name;
    void *addr;
};

/* Tables of overrides, each terminated by {NULL, NULL}. */
extern const struct shim_sym shim_libc_syms[];
extern const struct shim_sym shim_pthread_syms[];
extern const struct shim_sym shim_net_syms[];
extern const struct shim_sym shim_cxx_syms[];
extern const struct shim_sym shim_usb_syms[];
extern const struct shim_sym shim_fstream_syms[];

/* Translate a macOS errno value to its Linux equivalent. */
int shim_errno_to_linux(int e);
/* Store the Linux errno for the current macOS errno and return -1. */
long shim_fail(void);

/* Directory holding CalFile/ and config overrides (see htraapi_mac.c). */
const char *shim_data_dir(void);

/* Load an optional vendor plugin (e.g. libDigitalSigDemod.so) by basename. */
void *shim_dlopen_elf(const char *name);
void *shim_dlsym_elf(void *handle, const char *name);

extern int shim_trace;
#define SHIM_TRACE(...)                                                        \
    do {                                                                       \
        if (shim_trace)                                                        \
            fprintf(stderr, "[htraapi-mac] " __VA_ARGS__);                     \
    } while (0)

#endif
