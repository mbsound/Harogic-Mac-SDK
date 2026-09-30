/*
 * Minimal loader for aarch64 Linux ELF shared objects on arm64 macOS.
 *
 * Maps an ET_DYN object, applies RELATIVE/ABS64/GLOB_DAT/JUMP_SLOT/TLSDESC
 * relocations through a caller-supplied resolver, applies x18 patches,
 * registers .eh_frame with the system unwinder and runs initializers.
 */
#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stddef.h>
#include <stdint.h>

struct x18_patch {
    uint64_t offset;   /* instruction offset in the image */
    uint32_t old_insn; /* expected original encoding */
    uint32_t new_insn; /* instruction with x18 replaced by scratch */
    uint64_t target;   /* image offset: branch target (kinds 1-2) or address (3) */
    uint8_t kind;      /* 0 plain, 1 cbz/cbnz, 2 tbz/tbnz, 3 adr/adrp */
    uint8_t scratch;   /* GPR parked while the stub runs */
    uint8_t vreg;      /* SIMD register holding "x18" */
    uint8_t treg;      /* SIMD register used to park the scratch GPR */
};

struct elf_object;

enum {
    ELF_SYM_TLS = 1,  /* STT_TLS: resolve to an emutls control variable */
    ELF_SYM_WEAK = 2, /* weak reference: NULL is acceptable */
    ELF_SYM_FUNC = 4, /* STT_FUNC */
};

/* Returns the address for an undefined symbol, or NULL if it cannot be
 * resolved. `name` has any @VERSION suffix already stripped. For ELF_SYM_TLS
 * the resolver must return the address of an emutls control variable
 * (__emutls_v.<name>). */
typedef void *(*elf_resolver_fn)(const char *name, unsigned flags, void *ctx);

struct elf_load_opts {
    elf_resolver_fn resolve;
    void *ctx;
    const struct x18_patch *patches;
    size_t npatches;
    /* Called with the emutls control variable; returns this thread's address. */
    void *(*tls_get_address)(void *control);
};

/* Load an object from an in-memory image. `name` is used for diagnostics and
 * dl_iterate_phdr. The image is copied; the buffer may be freed afterwards. */
struct elf_object *elf_load(const char *name, const void *image, size_t size,
                            const struct elf_load_opts *opts, char *err, size_t errlen);

/* Run DT_INIT / DT_INIT_ARRAY. Separate from elf_load so that every object
 * can be relocated before any constructor runs. */
void elf_run_init(struct elf_object *obj);

/* Look up a defined global or weak symbol exported by the object. */
void *elf_sym(const struct elf_object *obj, const char *name);

/* Iterate exported functions (callback returns nonzero to stop). */
void elf_foreach_export(const struct elf_object *obj,
                        int (*cb)(const char *name, void *addr, void *ctx), void *ctx);

const char *elf_name(const struct elf_object *obj);
uintptr_t elf_base(const struct elf_object *obj);
const void *elf_phdrs(const struct elf_object *obj, uint16_t *count);

/* Loaded objects, in load order. */
size_t elf_count(void);
struct elf_object *elf_at(size_t i);

/* Object whose image contains addr, or NULL. */
struct elf_object *elf_find(uintptr_t addr);

/* Describe addr as "object!symbol+0xoff (0xoffset)" using .symtab when
 * available. Returns 0 if addr is not inside a loaded object. */
int elf_describe(uintptr_t addr, char *buf, size_t n);

#endif
