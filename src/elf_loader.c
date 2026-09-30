/*
 * Minimal aarch64 ELF shared-object loader for arm64 macOS. See elf_loader.h.
 */
#include "elf_loader.h"

#include <libkern/OSCacheControl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- ELF definitions (macOS has no <elf.h>) ---------------------------- */

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

typedef struct {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} Elf64_Shdr;

typedef struct {
    uint32_t st_name;
    unsigned char st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} Elf64_Sym;

typedef struct {
    uint64_t r_offset, r_info;
    int64_t r_addend;
} Elf64_Rela;

typedef struct {
    int64_t d_tag;
    uint64_t d_val;
} Elf64_Dyn;

enum {
    ET_DYN = 3, EM_AARCH64 = 183,
    PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7, PT_GNU_EH_FRAME = 0x6474e550,
    PF_X = 1, PF_W = 2,
    SHT_SYMTAB = 2, SHT_DYNSYM = 11,
    DT_NULL = 0, DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7,
    DT_RELASZ = 8, DT_INIT = 12, DT_JMPREL = 23, DT_INIT_ARRAY = 25,
    DT_INIT_ARRAYSZ = 27, DT_TEXTREL = 22,
    STB_GLOBAL = 1, STB_WEAK = 2, STT_FUNC = 2, STT_TLS = 6, STT_GNU_IFUNC = 10,
    SHN_UNDEF = 0,
    R_AARCH64_NONE = 0, R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025,
    R_AARCH64_JUMP_SLOT = 1026, R_AARCH64_RELATIVE = 1027, R_AARCH64_TLSDESC = 1031,
};

#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define ELF64_R_SYM(i) ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)(i))

/* ---- Object bookkeeping ------------------------------------------------ */

struct export_entry {
    const char *name;
    uintptr_t addr;
    int is_func;
};

struct elf_object {
    char *name;
    uint8_t *base;        /* image base (vaddr 0) */
    size_t image_size;    /* bytes covered by PT_LOAD segments */
    size_t map_size;      /* image + stub area, page aligned */
    Elf64_Phdr *phdrs;
    uint16_t phnum;
    const Elf64_Sym *dynsym;
    size_t nsyms;
    const char *dynstr;
    struct export_entry *exports; /* open-addressing hash table */
    size_t exports_cap;
    const Elf64_Sym *symtab;      /* full symbol table (points into the image) */
    size_t nsymtab;
    const char *strtab;
    uint64_t init, init_array, init_arraysz;
    int initialized;
};

#define MAX_OBJECTS 16
static struct elf_object *g_objects[MAX_OBJECTS];
static size_t g_nobjects;

/* Used by the TLSDESC trampoline in shim_asm.S. */
void *(*elf_tls_get_address_fn)(void *control);
extern void elf_tlsdesc_dynamic(void);

extern void __register_frame(const void *fde);

static void set_err(char *err, size_t errlen, const char *fmt, ...) {
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static uint64_t hash_name(const char *s) {
    uint64_t h = 1469598103934665603ull;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 1099511628211ull;
    return h;
}

static void export_insert(struct elf_object *o, const char *name, uintptr_t addr, int is_func) {
    size_t mask = o->exports_cap - 1, i = hash_name(name) & mask;
    while (o->exports[i].name) {
        if (!strcmp(o->exports[i].name, name))
            return; /* first definition wins (GLOBAL before WEAK is not needed here) */
        i = (i + 1) & mask;
    }
    o->exports[i] = (struct export_entry){name, addr, is_func};
}

void *elf_sym(const struct elf_object *o, const char *name) {
    size_t mask = o->exports_cap - 1, i = hash_name(name) & mask;
    while (o->exports[i].name) {
        if (!strcmp(o->exports[i].name, name))
            return (void *)o->exports[i].addr;
        i = (i + 1) & mask;
    }
    return NULL;
}

void elf_foreach_export(const struct elf_object *o,
                        int (*cb)(const char *name, void *addr, void *ctx), void *ctx) {
    for (size_t i = 0; i < o->exports_cap; i++)
        if (o->exports[i].name && o->exports[i].is_func)
            if (cb(o->exports[i].name, (void *)o->exports[i].addr, ctx))
                return;
}

const char *elf_name(const struct elf_object *o) { return o->name; }
uintptr_t elf_base(const struct elf_object *o) { return (uintptr_t)o->base; }
const void *elf_phdrs(const struct elf_object *o, uint16_t *count) {
    *count = o->phnum;
    return o->phdrs;
}
size_t elf_count(void) { return g_nobjects; }
struct elf_object *elf_at(size_t i) { return i < g_nobjects ? g_objects[i] : NULL; }

struct elf_object *elf_find(uintptr_t addr) {
    for (size_t i = 0; i < g_nobjects; i++) {
        struct elf_object *o = g_objects[i];
        if (addr >= (uintptr_t)o->base && addr < (uintptr_t)o->base + o->map_size)
            return o;
    }
    return NULL;
}

/* ---- Instruction encodings --------------------------------------------- */

static uint32_t enc_b(uintptr_t from, uintptr_t to) {
    return 0x14000000u | ((uint32_t)((int64_t)(to - from) >> 2) & 0x3FFFFFFu);
}
static uint32_t enc_fmov_d_from_x(unsigned d, unsigned x) { return 0x9E670000u | (x << 5) | d; }
static uint32_t enc_fmov_x_from_d(unsigned x, unsigned d) { return 0x9E660000u | (d << 5) | x; }

static int in_b_range(uintptr_t from, uintptr_t to) {
    int64_t delta = (int64_t)(to - from);
    return delta >= -(1ll << 27) && delta < (1ll << 27);
}

/* ---- Loading ----------------------------------------------------------- */

static int apply_x18_patches(struct elf_object *o, const struct elf_load_opts *opts,
                             uint8_t *stubs, char *err, size_t errlen) {
    uint32_t *out = (uint32_t *)stubs;
    for (size_t i = 0; i < opts->npatches; i++) {
        const struct x18_patch *p = &opts->patches[i];
        uint32_t *site = (uint32_t *)(o->base + p->offset);
        if (p->offset + 4 > o->image_size || *site != p->old_insn) {
            set_err(err, errlen, "%s: x18 patch %zu does not match image (offset 0x%llx)",
                    o->name, i, (unsigned long long)p->offset);
            return -1;
        }
        uint32_t *stub = out;
        uintptr_t next = (uintptr_t)(site + 1);
        if (p->kind == 3) {
            /* adr/adrp x18: "x18" := absolute address, via a literal. */
            uint64_t value = (uint64_t)(uintptr_t)o->base + p->target;
            *out++ = enc_fmov_d_from_x(p->treg, p->scratch);
            *out++ = 0x58000000u | (4u << 5) | p->scratch; /* ldr xS, [pc, #16] */
            *out++ = enc_fmov_d_from_x(p->vreg, p->scratch);
            *out++ = enc_fmov_x_from_d(p->scratch, p->treg);
            *out = enc_b((uintptr_t)out, next);
            out++;
            memcpy(out, &value, 8);
            out += 2;
            if (!in_b_range((uintptr_t)site, (uintptr_t)stub) || !in_b_range((uintptr_t)(out - 3), next))
                goto range;
            *site = enc_b((uintptr_t)site, (uintptr_t)stub);
            continue;
        }
        *out++ = enc_fmov_d_from_x(p->treg, p->scratch);
        *out++ = enc_fmov_x_from_d(p->scratch, p->vreg);
        *out++ = p->new_insn;
        if (p->kind == 0) {
            *out++ = enc_fmov_d_from_x(p->vreg, p->scratch);
            *out++ = enc_fmov_x_from_d(p->scratch, p->treg);
            *out = enc_b((uintptr_t)out, next);
            out++;
        } else {
            /* new_insn branches +12 (to index 5) when taken. */
            *out++ = enc_fmov_x_from_d(p->scratch, p->treg);
            *out = enc_b((uintptr_t)out, next);
            out++;
            *out++ = enc_fmov_x_from_d(p->scratch, p->treg);
            uintptr_t target = (uintptr_t)o->base + p->target;
            if (!in_b_range((uintptr_t)out, target))
                goto range;
            *out = enc_b((uintptr_t)out, target);
            out++;
        }
        if (!in_b_range((uintptr_t)site, (uintptr_t)stub) || !in_b_range((uintptr_t)out, next))
            goto range;
        *site = enc_b((uintptr_t)site, (uintptr_t)stub);
        continue;
    range:
        set_err(err, errlen, "%s: x18 stub out of branch range", o->name);
        return -1;
    }
    return 0;
}

/* TLS in this port is only reached through TLSDESC, whose resolver returns an
 * absolute address. The code then adds TPIDR_EL0, which on macOS holds the CPU
 * number, so replace every `mrs xN, tpidr_el0` with `movz xN, #0`. */
static void patch_thread_pointer_reads(struct elf_object *o) {
    for (uint16_t i = 0; i < o->phnum; i++) {
        Elf64_Phdr *ph = &o->phdrs[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X))
            continue;
        uint32_t *w = (uint32_t *)(o->base + ph->p_vaddr);
        for (size_t k = 0; k < ph->p_filesz / 4; k++)
            if ((w[k] & 0xFFFFFFE0u) == 0xD53BD040u)
                w[k] = 0xD2800000u | (w[k] & 31);
    }
}

static void register_eh_frame(struct elf_object *o, const uint8_t *file, const Elf64_Ehdr *eh) {
    if (!eh->e_shoff || eh->e_shstrndx >= eh->e_shnum)
        return;
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(file + eh->e_shoff);
    const char *shstr = (const char *)file + sh[eh->e_shstrndx].sh_offset;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (strcmp(shstr + sh[i].sh_name, ".eh_frame"))
            continue;
        const uint8_t *p = o->base + sh[i].sh_addr, *end = p + sh[i].sh_size;
        while (p + 8 <= end) {
            uint32_t len = *(const uint32_t *)p;
            if (len == 0 || len == 0xFFFFFFFFu)
                break;
            if (*(const uint32_t *)(p + 4) != 0) /* CIE id 0 => CIE; otherwise FDE */
                __register_frame(p);
            p += 4 + len;
        }
    }
}

struct elf_object *elf_load(const char *name, const void *image, size_t size,
                            const struct elf_load_opts *opts, char *err, size_t errlen) {
    const uint8_t *file = image;
    const Elf64_Ehdr *eh = image;
    if (size < sizeof *eh || memcmp(eh->e_ident, "\177ELF", 4) || eh->e_ident[4] != 2 ||
        eh->e_type != ET_DYN || eh->e_machine != EM_AARCH64) {
        set_err(err, errlen, "%s: not an aarch64 ELF64 shared object", name);
        return NULL;
    }
    if (g_nobjects == MAX_OBJECTS) {
        set_err(err, errlen, "too many ELF objects");
        return NULL;
    }

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(file + eh->e_phoff);
    uint64_t hi = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > hi)
            hi = ph[i].p_vaddr + ph[i].p_memsz;
        if (ph[i].p_type == PT_TLS) {
            set_err(err, errlen, "%s: objects defining TLS are not supported", name);
            return NULL;
        }
    }
    size_t page = (size_t)getpagesize();
    size_t image_size = (hi + page - 1) & ~(page - 1);
    size_t stub_size = (opts->npatches * 7 * 4 + page - 1) & ~(page - 1);
    size_t map_size = image_size + stub_size;

    uint8_t *base = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) {
        set_err(err, errlen, "%s: mmap failed", name);
        return NULL;
    }
    for (uint16_t i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD)
            memcpy(base + ph[i].p_vaddr, file + ph[i].p_offset, ph[i].p_filesz);

    struct elf_object *o = calloc(1, sizeof *o);
    o->name = strdup(name);
    o->base = base;
    o->image_size = image_size;
    o->map_size = map_size;
    o->phnum = eh->e_phnum;
    o->phdrs = malloc(sizeof(Elf64_Phdr) * eh->e_phnum);
    memcpy(o->phdrs, ph, sizeof(Elf64_Phdr) * eh->e_phnum);

    /* Dynamic section. */
    const Elf64_Dyn *dyn = NULL;
    for (uint16_t i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC)
            dyn = (const Elf64_Dyn *)(base + ph[i].p_vaddr);
    if (!dyn) {
        set_err(err, errlen, "%s: no PT_DYNAMIC", name);
        return NULL;
    }
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
    for (const Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_STRTAB: o->dynstr = (const char *)base + d->d_val; break;
        case DT_SYMTAB: o->dynsym = (const Elf64_Sym *)(base + d->d_val); break;
        case DT_RELA: rela = d->d_val; break;
        case DT_RELASZ: relasz = d->d_val; break;
        case DT_JMPREL: jmprel = d->d_val; break;
        case DT_PLTRELSZ: pltrelsz = d->d_val; break;
        case DT_INIT: o->init = d->d_val; break;
        case DT_INIT_ARRAY: o->init_array = d->d_val; break;
        case DT_INIT_ARRAYSZ: o->init_arraysz = d->d_val; break;
        case DT_TEXTREL:
            set_err(err, errlen, "%s: DT_TEXTREL not supported", name);
            return NULL;
        }
    }

    /* Symbol count from the section headers (.dynsym); keep .symtab for
     * diagnostics (the caller's image outlives the object). */
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(file + eh->e_shoff);
    for (uint16_t i = 0; eh->e_shoff && i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_DYNSYM)
            o->nsyms = sh[i].sh_size / sizeof(Elf64_Sym);
        if (sh[i].sh_type == SHT_SYMTAB && sh[i].sh_link < eh->e_shnum) {
            o->symtab = (const Elf64_Sym *)(file + sh[i].sh_offset);
            o->nsymtab = sh[i].sh_size / sizeof(Elf64_Sym);
            o->strtab = (const char *)file + sh[sh[i].sh_link].sh_offset;
        }
    }
    if (!o->nsyms) {
        set_err(err, errlen, "%s: cannot determine dynamic symbol count", name);
        return NULL;
    }

    o->exports_cap = 1;
    while (o->exports_cap < o->nsyms * 2)
        o->exports_cap <<= 1;
    o->exports = calloc(o->exports_cap, sizeof *o->exports);
    for (size_t i = 1; i < o->nsyms; i++) {
        const Elf64_Sym *s = &o->dynsym[i];
        unsigned bind = ELF64_ST_BIND(s->st_info), type = ELF64_ST_TYPE(s->st_info);
        if (s->st_shndx == SHN_UNDEF || (bind != STB_GLOBAL && bind != STB_WEAK))
            continue;
        if (type == STT_GNU_IFUNC) {
            set_err(err, errlen, "%s: IFUNC symbols not supported", name);
            return NULL;
        }
        if (type == STT_TLS)
            continue;
        export_insert(o, o->dynstr + s->st_name, (uintptr_t)base + s->st_value, type == STT_FUNC);
    }

    /* Relocations. Undefined symbols are resolved once each. */
    void **resolved = calloc(o->nsyms, sizeof(void *));
    char *done = calloc(o->nsyms, 1);
    char missing[1024] = "";
    int nmissing = 0;
    const struct { uint64_t off, size; } tables[2] = {{rela, relasz}, {jmprel, pltrelsz}};
    for (int t = 0; t < 2; t++) {
        const Elf64_Rela *r = (const Elf64_Rela *)(base + tables[t].off);
        size_t n = tables[t].size / sizeof(Elf64_Rela);
        for (size_t k = 0; tables[t].size && k < n; k++) {
            uint64_t *where = (uint64_t *)(base + r[k].r_offset);
            uint32_t type = ELF64_R_TYPE(r[k].r_info), si = ELF64_R_SYM(r[k].r_info);
            const Elf64_Sym *s = si ? &o->dynsym[si] : NULL;
            uintptr_t S = 0;
            if (s && type != R_AARCH64_RELATIVE) {
                if (!done[si]) {
                    done[si] = 1;
                    int tls = ELF64_ST_TYPE(s->st_info) == STT_TLS;
                    if (s->st_shndx != SHN_UNDEF && !tls) {
                        resolved[si] = base + s->st_value;
                    } else {
                        int weak = ELF64_ST_BIND(s->st_info) == STB_WEAK;
                        unsigned flags = (tls ? ELF_SYM_TLS : 0) | (weak ? ELF_SYM_WEAK : 0) |
                                         (ELF64_ST_TYPE(s->st_info) == STT_FUNC ? ELF_SYM_FUNC : 0);
                        resolved[si] = opts->resolve(o->dynstr + s->st_name, flags, opts->ctx);
                        if (!resolved[si] && !weak) {
                            if (nmissing++ < 12) {
                                strlcat(missing, " ", sizeof missing);
                                strlcat(missing, o->dynstr + s->st_name, sizeof missing);
                            }
                        }
                    }
                }
                S = (uintptr_t)resolved[si];
            }
            switch (type) {
            case R_AARCH64_NONE: break;
            case R_AARCH64_RELATIVE: *where = (uintptr_t)base + r[k].r_addend; break;
            case R_AARCH64_ABS64:
            case R_AARCH64_GLOB_DAT:
            case R_AARCH64_JUMP_SLOT: *where = S + r[k].r_addend; break;
            case R_AARCH64_TLSDESC:
                if (!opts->tls_get_address || !S) {
                    set_err(err, errlen, "%s: unresolved TLS symbol %s", name,
                            s ? o->dynstr + s->st_name : "?");
                    return NULL;
                }
                elf_tls_get_address_fn = opts->tls_get_address;
                where[0] = (uintptr_t)elf_tlsdesc_dynamic;
                where[1] = S; /* emutls control variable */
                break;
            default:
                set_err(err, errlen, "%s: unsupported relocation type %u", name, type);
                return NULL;
            }
        }
    }
    free(resolved);
    free(done);
    if (nmissing) {
        set_err(err, errlen, "%s: %d unresolved symbol(s):%s%s", name, nmissing, missing,
                nmissing > 12 ? " ..." : "");
        return NULL;
    }

    patch_thread_pointer_reads(o);
    if (opts->npatches && apply_x18_patches(o, opts, base + image_size, err, errlen))
        return NULL;

    /* Protections: executable segments RX, the rest RW, stub area RX. */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X))
            continue;
        uintptr_t lo = ph[i].p_vaddr & ~(page - 1);
        uintptr_t end = (ph[i].p_vaddr + ph[i].p_memsz + page - 1) & ~(page - 1);
        for (uint16_t j = 0; j < eh->e_phnum; j++) /* refuse RX/RW page sharing */
            if (j != i && ph[j].p_type == PT_LOAD && ph[j].p_vaddr < end &&
                ph[j].p_vaddr + ph[j].p_memsz > lo) {
                set_err(err, errlen, "%s: code and data share a page", name);
                return NULL;
            }
        if (mprotect(base + lo, end - lo, PROT_READ | PROT_EXEC)) {
            set_err(err, errlen, "%s: mprotect RX failed", name);
            return NULL;
        }
        sys_icache_invalidate(base + lo, end - lo);
    }
    if (stub_size) {
        if (mprotect(base + image_size, stub_size, PROT_READ | PROT_EXEC)) {
            set_err(err, errlen, "%s: mprotect stubs failed", name);
            return NULL;
        }
        sys_icache_invalidate(base + image_size, stub_size);
    }

    register_eh_frame(o, file, eh);
    g_objects[g_nobjects++] = o;
    return o;
}

void elf_run_init(struct elf_object *o) {
    if (o->initialized)
        return;
    o->initialized = 1;
    if (o->init)
        ((void (*)(void))(o->base + o->init))();
    uintptr_t *arr = (uintptr_t *)(o->base + o->init_array);
    for (size_t i = 0; o->init_array && i < o->init_arraysz / sizeof(uintptr_t); i++)
        if (arr[i] && arr[i] != (uintptr_t)-1)
            ((void (*)(int, char **, char **))arr[i])(0, NULL, NULL);
}

int elf_describe(uintptr_t addr, char *buf, size_t n) {
    struct elf_object *o = elf_find(addr);
    if (!o) return 0;
    uint64_t off = addr - (uintptr_t)o->base;
    if (off >= o->image_size) {
        snprintf(buf, n, "%s!<x18 stub> (0x%llx)", o->name, (unsigned long long)off);
        return 1;
    }
    const Elf64_Sym *tabs[2] = {o->symtab, o->dynsym};
    const char *strs[2] = {o->strtab, o->dynstr};
    size_t counts[2] = {o->nsymtab, o->nsyms};
    const char *best = NULL;
    uint64_t best_val = 0;
    for (int t = 0; t < 2 && !best; t++)
        for (size_t i = 0; tabs[t] && i < counts[t]; i++) {
            const Elf64_Sym *s = &tabs[t][i];
            if (ELF64_ST_TYPE(s->st_info) != STT_FUNC || s->st_shndx == SHN_UNDEF) continue;
            if (s->st_value <= off && s->st_value >= best_val) {
                best_val = s->st_value;
                best = strs[t] + s->st_name;
            }
        }
    snprintf(buf, n, "%s!%s+0x%llx (0x%llx)", o->name, best ? best : "?",
             (unsigned long long)(off - best_val), (unsigned long long)off);
    return 1;
}
