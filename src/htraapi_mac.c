/*
 * libhtraapi.dylib for arm64 macOS.
 *
 * On load, maps the vendor's aarch64 Linux ELF objects (embedded in this
 * dylib), links their imports against macOS equivalents and the glibc shim
 * layer, and points this dylib's exported API trampolines (exports.S) at the
 * vendor implementations.
 */
#include "shim.h"

#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "elf_loader.h"
#include "gen/images.h"

void shim_cxx_init(void *libstdcxx);

/* Trampoline slots, defined in gen/exports.S. */
extern void *htra_export_slots[];
extern const char *const htra_export_names[];
extern const unsigned htra_export_count;

static void *g_stdcxx;
static char g_data_dir[PATH_MAX];
static char g_init_error[1024];

/* ---- Paths ------------------------------------------------------------- */

const char *shim_data_dir(void) { return g_data_dir; }

static void init_data_dir(void) {
    const char *env = getenv("HTRAAPI_DATA_DIR");
    if (env && *env) {
        strlcpy(g_data_dir, env, sizeof g_data_dir);
        return;
    }
    Dl_info info;
    if (dladdr((void *)init_data_dir, &info) && info.dli_fname) {
        char *p = realpath(info.dli_fname, NULL);
        if (p) {
            char *slash = strrchr(p, '/');
            if (slash) *slash = 0;
            strlcpy(g_data_dir, p, sizeof g_data_dir);
            free(p);
            return;
        }
    }
    strlcpy(g_data_dir, ".", sizeof g_data_dir);
}

/* ---- Symbol resolution ------------------------------------------------- */

/* Linux int64_t/streamoff is `long` ('l'); on Darwin it is `long long` ('x'). */
static const char *const renames[][2] = {
    {"_ZNSi5seekgElSt12_Ios_Seekdir", "_ZNSi5seekgExSt12_Ios_Seekdir"},
    {"_ZNSo5seekpElSt12_Ios_Seekdir", "_ZNSo5seekpExSt12_Ios_Seekdir"},
};

/* libc/libm functions whose ABI is identical on glibc-aarch64 and macOS. */
static const char *const passthrough[] = {
    /* math */
    "atan2", "atan2f", "atanf", "cabs", "cabsf", "cos", "cosf", "exp", "expf", "hypot",
    "log", "log10", "log10f", "log2", "log2f", "logf", "modf", "pow", "sin", "sinf",
    "sqrt", "sqrtf", "ccosf", "cexpf", "coshf", "csinf", "csqrtf", "erfcf", "erff",
    "lgamma", "lgammaf", "powf", "tanf", "tanhf", "tgamma", "floor", "ceil", "round",
    "fabs", "fmod", "exp2", "atan", "asin", "acos", "tan", "tanh", "sinh", "cosh",
    "llround", "lround", "expm1", "log1p", "cbrt", "trunc", "fmin", "fmax", "fminf", "fmaxf",
    "roundf", "floorf", "ceilf", "fabsf", "fmodf", "exp2f", "asinf", "acosf",
    /* memory and strings */
    "memcmp", "memcpy", "memmove", "memset", "memchr", "strchr", "strrchr", "strcmp",
    "strlen", "strncmp", "strncpy", "strcpy", "strcat", "strstr", "strtok", "strtod",
    "strtof", "strtol", "strtoul", "strtoll", "strtoull", "atoi", "atof", "isalpha",
    "isspace", "isdigit", "toupper", "tolower",
    /* allocation and misc */
    "malloc", "free", "calloc", "realloc", "posix_memalign", "qsort", "div", "rand",
    "srand", "abort", "exit", "getenv", "pipe",
    /* stack protector: canary variable and failure handler */
    "__stack_chk_guard", "__stack_chk_fail",
    /* threads (pthread_t is 8 bytes on both) */
    "pthread_join", "pthread_detach", "pthread_self", "sched_yield",
    /* stdio (FILE* is opaque; stdout/stderr are shimmed) */
    "fclose", "ferror", "feof", "fflush", "fgetc", "fgets", "fputc", "fputs", "fread",
    "fseek", "ftell", "fwrite", "puts", "putchar", "perror", "popen", "pclose", "rewind",
    /* time (struct tm/timespec layouts match) */
    "time", "gmtime_r", "localtime", "localtime_r", "timegm", "mktime", "nanosleep",
    "sleep", "usleep", "pause", "strftime",
    /* C++ runtime helpers that live in libSystem */
    "__cxa_atexit", "__cxa_finalize",
};

static int is_cxx_runtime(const char *n) {
    return !strncmp(n, "_Z", 2) || !strncmp(n, "__cxa_", 6) || !strncmp(n, "__gxx_", 6) ||
           !strcmp(n, "__dynamic_cast") || !strncmp(n, "_Unwind_", 8) ||
           !strcmp(n, "__once_proxy") || !strcmp(n, "__divdc3") || !strcmp(n, "__muldc3") ||
           !strcmp(n, "__divsc3") || !strcmp(n, "__mulsc3");
}

static void *find_in_table(const struct shim_sym *t, const char *name) {
    for (; t->name; t++)
        if (!strcmp(t->name, name)) return t->addr;
    return NULL;
}

static void *resolve_raw(const char *name, unsigned flags) {
    for (size_t i = 0; i < sizeof renames / sizeof renames[0]; i++)
        if (!strcmp(name, renames[i][0])) name = renames[i][1];

    if (flags & ELF_SYM_TLS) {
        char buf[512];
        snprintf(buf, sizeof buf, "__emutls_v.%s", name);
        return dlsym(g_stdcxx, buf);
    }

    static const struct shim_sym *const tables[] = {
        shim_libc_syms, shim_pthread_syms, shim_net_syms, shim_cxx_syms, shim_usb_syms,
        shim_fstream_syms, shim_alloc_syms,
    };
    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; i++) {
        void *p = find_in_table(tables[i], name);
        if (p) return p;
    }

    for (size_t i = 0; i < elf_count(); i++) {
        void *p = elf_sym(elf_at(i), name);
        if (p) return p;
    }

    if (is_cxx_runtime(name)) {
        void *p = dlsym(g_stdcxx, name);
        /* std::experimental::filesystem ships only as a static archive
         * (libstdc++fs.a); it is linked into, and exported from, this dylib. */
        if (!p) p = dlsym(RTLD_SELF, name);
        if (p) return p;
    }
    if (!strncmp(name, "libusb_", 7))
        return dlsym(RTLD_DEFAULT, name);
    for (size_t i = 0; i < sizeof passthrough / sizeof passthrough[0]; i++)
        if (!strcmp(name, passthrough[i])) return dlsym(RTLD_DEFAULT, name);

    if (!(flags & ELF_SYM_WEAK)) SHIM_TRACE("unresolved import: %s\n", name);
    return NULL;
}

/* ---- Import call tracing (HTRAAPI_TRACE_IMPORTS=1) --------------------- */

extern void elf_trace_common(void);

/* Called from elf_trace_common with the import name before each call. */
void elf_trace_log(const char *name) {
    fprintf(stderr, "[htraapi-mac] -> %s\n", name);
}

static const char *const trace_quiet[] = {
    "memcpy", "memset", "memmove", "memcmp", "strlen", "strcmp", "strncmp", "malloc", "free",
    "calloc", "realloc", "pthread_mutex_lock", "pthread_mutex_unlock", "__errno_location",
    "sqrt", "sqrtf", "log10", "log10f", "pow", "sin", "cos", "sinf", "cosf", "exp", "expf",
    "atan2", "atan2f", "hypot", "logf", "log", "floor",
};

static int trace_wanted(const char *name) {
    if (!strncmp(name, "_Z", 2) || !strncmp(name, "__cxa", 5)) return 0; /* C++ runtime noise */
    for (size_t i = 0; i < sizeof trace_quiet / sizeof trace_quiet[0]; i++)
        if (!strcmp(name, trace_quiet[i])) return 0;
    return 1;
}

/* Thunk: ldr x16,T; ldr x17,N; ldr x9,C; br x9; T; N; C  (x9 is scratch at a call). */
static void *make_trace_thunk(const char *name, void *target) {
    static uint32_t *page, *cur, *end;
    if (!page || cur + 10 > end) {
        size_t sz = 1 << 16;
        page = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (page == MAP_FAILED) return target;
        cur = page;
        end = page + sz / 4;
    }
    mprotect(page, (size_t)(end - page) * 4, PROT_READ | PROT_WRITE);
    uint32_t *t = cur;
    t[0] = 0x58000000u | (4 << 5) | 16; /* ldr x16, #16 */
    t[1] = 0x58000000u | (5 << 5) | 17; /* ldr x17, #20 */
    t[2] = 0x58000000u | (6 << 5) | 9;  /* ldr x9,  #24 */
    t[3] = 0xD61F0120u;                 /* br x9 */
    uint64_t lits[3] = {(uintptr_t)target, (uintptr_t)strdup(name), (uintptr_t)elf_trace_common};
    memcpy(&t[4], lits, sizeof lits);
    cur += 10;
    mprotect(page, (size_t)(end - page) * 4, PROT_READ | PROT_EXEC);
    sys_icache_invalidate(t, 40);
    return t;
}

static int g_trace_imports;

static void *resolve(const char *name, unsigned flags, void *ctx) {
    (void)ctx;
    void *p = resolve_raw(name, flags);
    if (p && g_trace_imports && (flags & ELF_SYM_FUNC) && trace_wanted(name))
        p = make_trace_thunk(name, p);
    return p;
}

/* ---- Loading ----------------------------------------------------------- */

static struct elf_object *load_image(const struct embedded_image *img) {
    struct elf_load_opts opts = {
        .resolve = resolve,
        .patches = img->patches,
        .npatches = img->npatches,
        .tls_get_address = dlsym(g_stdcxx, "__emutls_get_address"),
    };
    char err[1024];
    struct elf_object *o = elf_load(img->name, img->start, (size_t)(img->end - img->start),
                                    &opts, err, sizeof err);
    if (!o) {
        snprintf(g_init_error, sizeof g_init_error, "%s", err);
        fprintf(stderr, "htraapi-mac: %s\n", err);
    }
    return o;
}

void *shim_dlopen_elf(const char *name) {
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    for (size_t i = 0; i < embedded_image_count; i++) {
        const struct embedded_image *img = &embedded_images[i];
        if (strcmp(img->name, base)) continue;
        for (size_t k = 0; k < elf_count(); k++)
            if (!strcmp(elf_name(elf_at(k)), base)) return elf_at(k);
        struct elf_object *o = load_image(img);
        if (o) elf_run_init(o);
        return o;
    }
    return NULL; /* optional vendor plugin not available */
}

void *shim_dlsym_elf(void *handle, const char *name) {
    return elf_sym(handle, name);
}

/* The dylib links GCC's libstdc++ directly; get a handle to that same image
 * (never load a second copy). */
static void *open_linked_libstdcxx(char *path, size_t n) {
    Dl_info info;
    void *cout = dlsym(RTLD_DEFAULT, "_ZSt4cout");
    if (!cout || !dladdr(cout, &info)) return NULL;
    strlcpy(path, info.dli_fname, n);
    return dlopen(path, RTLD_NOW | RTLD_NOLOAD);
}

/* ---- Crash reporting (HTRAAPI_TRACE=1) --------------------------------- */

static void describe(const char *label, uintptr_t a) {
    char buf[512];
    if (elf_describe(a, buf, sizeof buf)) {
        fprintf(stderr, "  %-4s %016lx  %s\n", label, (unsigned long)a, buf);
        return;
    }
    Dl_info info;
    if (dladdr((void *)a, &info) && info.dli_sname)
        fprintf(stderr, "  %-4s %016lx  %s!%s+0x%lx\n", label, (unsigned long)a,
                strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname,
                info.dli_sname, (unsigned long)(a - (uintptr_t)info.dli_saddr));
    else
        fprintf(stderr, "  %-4s %016lx\n", label, (unsigned long)a);
}

static void crash_handler(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    fprintf(stderr, "\n[htraapi-mac] signal %d, fault address %p\n", sig, si->si_addr);
    describe("pc", uc->uc_mcontext->__ss.__pc);
    describe("lr", uc->uc_mcontext->__ss.__lr);
    for (int i = 0; i < 29; i++)
        fprintf(stderr, "  x%-2d %016llx%s", i, uc->uc_mcontext->__ss.__x[i], i % 4 == 3 ? "\n" : "");
    fprintf(stderr, "\n  stack words pointing into vendor code:\n");
    uintptr_t *sp = (uintptr_t *)uc->uc_mcontext->__ss.__sp;
    for (int i = 0, shown = 0; i < 512 && shown < 12; i++)
        if (elf_find(sp[i])) {
            describe("", sp[i]);
            shown++;
        }
    signal(sig, SIG_DFL);
}

static void install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

static int missing_export(void) {
    fprintf(stderr, "htraapi-mac: called an API function that failed to load: %s\n",
            g_init_error[0] ? g_init_error : "(unknown)");
    return -1;
}

__attribute__((constructor)) static void htraapi_mac_init(void) {
    shim_trace = getenv("HTRAAPI_TRACE") != NULL;
    g_trace_imports = getenv("HTRAAPI_TRACE_IMPORTS") != NULL;
    if (shim_trace) install_crash_handler();
    init_data_dir();
    for (unsigned i = 0; i < htra_export_count; i++)
        htra_export_slots[i] = (void *)missing_export;

    char path[PATH_MAX];
    g_stdcxx = open_linked_libstdcxx(path, sizeof path);
    if (!g_stdcxx) {
        snprintf(g_init_error, sizeof g_init_error, "cannot locate GCC libstdc++ (%s)", dlerror());
        fprintf(stderr, "htraapi-mac: %s\n", g_init_error);
        return;
    }
    shim_cxx_init(g_stdcxx);

    /* Load everything that is not a dlopen()-only plugin, in order, then run
     * constructors once all are relocated. */
    struct elf_object *loaded[8];
    size_t nloaded = 0;
    for (size_t i = 0; i < embedded_image_count; i++) {
        if (embedded_images[i].plugin) continue;
        struct elf_object *o = load_image(&embedded_images[i]);
        if (!o) return;
        loaded[nloaded++] = o;
    }
    for (size_t i = 0; i < nloaded; i++)
        elf_run_init(loaded[i]);

    unsigned missing = 0;
    for (unsigned i = 0; i < htra_export_count; i++) {
        void *p = NULL;
        for (size_t k = nloaded; k-- > 0 && !p;)
            p = elf_sym(loaded[k], htra_export_names[i]);
        if (p) htra_export_slots[i] = p;
        else missing++;
    }
    SHIM_TRACE("loaded %zu ELF objects, %u exports (%u missing), data dir %s\n", nloaded,
               htra_export_count, missing, g_data_dir);
}
