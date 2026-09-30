/*
 * glibc -> macOS shims: errno, stdio varargs, files, directories, dl*, misc.
 *
 * Only functions whose ABI or semantics differ are here. Functions with an
 * identical ABI are passed straight through by the resolver (htraapi_mac.c).
 */
#include "shim.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "elf_loader.h"

int shim_trace;

/* ---- errno ------------------------------------------------------------- */

/* Values 1..34 are identical on Linux and macOS. */
int shim_errno_to_linux(int e) {
    switch (e) {
    case EDEADLK: return 35;
    case EAGAIN: return 11;
    case EINPROGRESS: return 115;
    case EALREADY: return 114;
    case ENOTSOCK: return 88;
    case EDESTADDRREQ: return 89;
    case EMSGSIZE: return 90;
    case EPROTOTYPE: return 91;
    case ENOPROTOOPT: return 92;
    case EPROTONOSUPPORT: return 93;
    case ENOTSUP: return 95;
    case EOPNOTSUPP: return 95;
    case EAFNOSUPPORT: return 97;
    case EADDRINUSE: return 98;
    case EADDRNOTAVAIL: return 99;
    case ENETDOWN: return 100;
    case ENETUNREACH: return 101;
    case ENETRESET: return 102;
    case ECONNABORTED: return 103;
    case ECONNRESET: return 104;
    case ENOBUFS: return 105;
    case EISCONN: return 106;
    case ENOTCONN: return 107;
    case ESHUTDOWN: return 108;
    case ETIMEDOUT: return 110;
    case ECONNREFUSED: return 111;
    case EHOSTDOWN: return 112;
    case EHOSTUNREACH: return 113;
    case ELOOP: return 40;
    case ENAMETOOLONG: return 36;
    case ENOTEMPTY: return 39;
    case ENOLCK: return 37;
    case ENOSYS: return 38;
    case EOVERFLOW: return 75;
    case ECANCELED: return 125;
    case EILSEQ: return 84;
    default: return e;
    }
}

long shim_fail(void) {
    errno = shim_errno_to_linux(errno);
    return -1;
}

/* errno lives in the real macOS errno; values that matter are translated by
 * the wrappers that can fail with them. */
static int *lx___errno_location(void) { return __error(); }

static ssize_t lx_read(int fd, void *buf, size_t n) {
    ssize_t r = read(fd, buf, n);
    return r < 0 ? shim_fail() : r;
}
static ssize_t lx_write(int fd, const void *buf, size_t n) {
    ssize_t r = write(fd, buf, n);
    return r < 0 ? shim_fail() : r;
}
static int lx_close(int fd) { return close(fd) ? (int)shim_fail() : 0; }

static char *lx_strerror_r(int e, char *buf, size_t n) {
    strerror_r(e, buf, n); /* GNU variant returns the buffer */
    return buf;
}

__attribute__((noreturn)) static void lx___assert_fail(const char *expr, const char *file,
                                                       unsigned line, const char *fn) {
    fprintf(stderr, "htraapi: assertion failed: %s (%s:%u %s)\n", expr, file, line, fn ? fn : "");
    abort();
}

/* ---- varargs ----------------------------------------------------------- */

static uint64_t va_gp(lx_va_list *va) {
    uint64_t v;
    if (va->gr_offs < 0) {
        memcpy(&v, (char *)va->gr_top + va->gr_offs, 8);
        va->gr_offs += 8;
    } else {
        memcpy(&v, va->stack, 8);
        va->stack = (char *)va->stack + 8;
    }
    return v;
}

static double va_fp(lx_va_list *va) {
    double v;
    if (va->vr_offs < 0) {
        memcpy(&v, (char *)va->vr_top + va->vr_offs, 8);
        va->vr_offs += 16;
    } else {
        memcpy(&v, va->stack, 8);
        va->stack = (char *)va->stack + 8;
    }
    return v;
}

/* Linux long double is IEEE binary128; convert to double. */
static double va_quad(lx_va_list *va) {
    uint64_t q[2];
    if (va->vr_offs < 0) {
        memcpy(q, (char *)va->vr_top + va->vr_offs, 16);
        va->vr_offs += 16;
    } else {
        uintptr_t p = ((uintptr_t)va->stack + 15) & ~(uintptr_t)15;
        memcpy(q, (void *)p, 16);
        va->stack = (void *)(p + 16);
    }
    int sign = (int)(q[1] >> 63);
    int exp = (int)((q[1] >> 48) & 0x7FFF);
    uint64_t mant_hi = q[1] & 0xFFFFFFFFFFFFull; /* top 48 of 112 mantissa bits */
    double m;
    if (exp == 0x7FFF)
        m = (mant_hi || q[0]) ? NAN : INFINITY;
    else if (exp == 0 && !mant_hi && !q[0])
        m = 0.0;
    else
        m = ldexp(1.0 + ldexp((double)mant_hi, -48) + ldexp((double)(q[0] >> 11), -101),
                  exp - 16383);
    return sign ? -m : m;
}

#define MAX_SLOTS 64

/* Walk a printf format, pulling each argument from a Linux va_list into an
 * array of 8-byte slots -- which is exactly an Apple arm64 va_list. The format
 * is copied to `out` with glibc-only conversions (%m, %n) rewritten. */
static int repack_printf(const char *fmt, lx_va_list *va, uint64_t *slots, char *out, size_t outsz) {
    int n = 0;
    size_t o = 0;
    int saved_errno = errno;
#define EMIT(c)                                                                \
    do {                                                                       \
        if (o + 1 < outsz)                                                     \
            out[o++] = (c);                                                    \
    } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            EMIT(*p);
            continue;
        }
        const char *spec = p++;
        if (*p == '%') {
            EMIT('%');
            EMIT('%');
            continue;
        }
        while (*p && strchr("-+ #0'I", *p))
            p++;
        if (*p == '*') {
            if (n < MAX_SLOTS) slots[n++] = (uint32_t)va_gp(va);
            p++;
        } else
            while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') {
            p++;
            if (*p == '*') {
                if (n < MAX_SLOTS) slots[n++] = (uint32_t)va_gp(va);
                p++;
            } else
                while (*p >= '0' && *p <= '9') p++;
        }
        int is_L = 0;
        while (*p && strchr("hlLqjzt", *p)) {
            if (*p == 'L') is_L = 1;
            p++;
        }
        char conv = *p;
        if (!conv) break;
        if (conv == 'm') { /* glibc: strerror(errno) */
            EMIT('%');
            EMIT('s');
            if (n < MAX_SLOTS) slots[n++] = (uint64_t)(uintptr_t)strerror(saved_errno);
            continue;
        }
        if (conv == 'n') { /* not allowed in dynamic formats on macOS */
            (void)va_gp(va);
            continue;
        }
        for (const char *q = spec; q <= p; q++)
            EMIT(*q);
        if (n >= MAX_SLOTS) continue;
        if (strchr("aAeEfFgG", conv)) {
            double d = is_L ? va_quad(va) : va_fp(va);
            memcpy(&slots[n++], &d, 8);
        } else {
            slots[n++] = va_gp(va);
        }
    }
    out[o < outsz ? o : outsz - 1] = 0;
#undef EMIT
    return n;
}

/* scanf: every non-suppressed conversion consumes one pointer. */
static void repack_scanf(const char *fmt, lx_va_list *va, uint64_t *slots) {
    int n = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        int suppress = 0;
        if (*p == '*') { suppress = 1; p++; }
        while (*p >= '0' && *p <= '9') p++;
        while (*p && strchr("hlLqjztm", *p)) p++;
        if (*p == '[') {
            p++;
            if (*p == '^') p++;
            if (*p == ']') p++;
            while (*p && *p != ']') p++;
        }
        if (!*p) break;
        if (!suppress && n < MAX_SLOTS) slots[n++] = va_gp(va);
    }
}

#define FMTBUF 4096

int lx_vsnprintf_impl(char *buf, size_t n, const char *fmt, lx_va_list *va) {
    uint64_t slots[MAX_SLOTS] = {0};
    char f[FMTBUF];
    repack_printf(fmt, va, slots, f, sizeof f);
    return vsnprintf(buf, n, f, (va_list)(void *)slots);
}

int lx_vfprintf_impl(FILE *fp, const char *fmt, lx_va_list *va) {
    uint64_t slots[MAX_SLOTS] = {0};
    char f[FMTBUF];
    repack_printf(fmt, va, slots, f, sizeof f);
    return vfprintf(fp, f, (va_list)(void *)slots);
}

int lx_vprintf_impl(const char *fmt, lx_va_list *va) { return lx_vfprintf_impl(stdout, fmt, va); }
int lx___vprintf_chk_impl(int flag, const char *fmt, lx_va_list *va) {
    (void)flag;
    return lx_vfprintf_impl(stdout, fmt, va);
}
int lx___vfprintf_chk_impl(FILE *fp, int flag, const char *fmt, lx_va_list *va) {
    (void)flag;
    return lx_vfprintf_impl(fp, fmt, va);
}
int lx___vsprintf_chk_impl(char *buf, int flag, size_t slen, const char *fmt, lx_va_list *va) {
    (void)flag;
    return lx_vsnprintf_impl(buf, slen, fmt, va);
}

int lx___vsnprintf_chk_impl(char *buf, size_t maxlen, int flag, size_t slen, const char *fmt,
                             lx_va_list *va) {
    (void)flag;
    if (slen < maxlen) abort(); /* glibc: __chk_fail */
    return lx_vsnprintf_impl(buf, maxlen, fmt, va);
}

int lx_vsscanf_impl(const char *s, const char *fmt, lx_va_list *va) {
    uint64_t slots[MAX_SLOTS] = {0};
    repack_scanf(fmt, va, slots);
    return vsscanf(s, fmt, (va_list)(void *)slots);
}
int lx_vfscanf_impl(FILE *fp, const char *fmt, lx_va_list *va) {
    uint64_t slots[MAX_SLOTS] = {0};
    repack_scanf(fmt, va, slots);
    return vfscanf(fp, fmt, (va_list)(void *)slots);
}

/* v* variants receive a pointer to the caller's va_list struct. */
static int lx_vsnprintf(char *buf, size_t n, const char *fmt, lx_va_list *va) {
    lx_va_list copy = *va;
    return lx_vsnprintf_impl(buf, n, fmt, &copy);
}
static int lx_vfprintf(FILE *fp, const char *fmt, lx_va_list *va) {
    lx_va_list copy = *va;
    return lx_vfprintf_impl(fp, fmt, &copy);
}
static int lx___vsnprintf_chk(char *buf, size_t maxlen, int flag, size_t slen, const char *fmt,
                              lx_va_list *va) {
    lx_va_list copy = *va;
    return lx___vsnprintf_chk_impl(buf, maxlen, flag, slen, fmt, &copy);
}
static int lx___vfprintf_chk(FILE *fp, int flag, const char *fmt, lx_va_list *va) {
    (void)flag;
    return lx_vfprintf(fp, fmt, va);
}

/* Entry points in shim_asm.S. */
extern int lx_printf(const char *, ...);
extern int lx_fprintf(FILE *, const char *, ...);
extern int lx_snprintf(char *, size_t, const char *, ...);
extern int lx_sscanf(const char *, const char *, ...);
extern int lx_fscanf(FILE *, const char *, ...);
extern int lx___printf_chk(int, const char *, ...);
extern int lx___fprintf_chk(FILE *, int, const char *, ...);
extern int lx___sprintf_chk(char *, int, size_t, const char *, ...);
extern int lx___snprintf_chk(char *, size_t, int, size_t, const char *, ...);

/* ---- Other _FORTIFY_SOURCE entry points --------------------------------- */

static void *lx___memcpy_chk(void *d, const void *s, size_t n, size_t dlen) {
    if (n > dlen) abort();
    return memcpy(d, s, n);
}
static ssize_t lx___read_chk(int fd, void *buf, size_t n, size_t buflen) {
    if (n > buflen) abort();
    return lx_read(fd, buf, n);
}
static size_t lx___fread_chk(void *p, size_t plen, size_t size, size_t n, FILE *fp) {
    if (size && n > plen / size) abort();
    return fread(p, size, n, fp);
}

/* ---- Paths ------------------------------------------------------------- */

/* The SDK's USB whitelist (VID/PID of supported analyzers). Linux installs it
 * as /etc/htrausb.conf; this is the vendor default, used when no override is
 * found next to the dylib (or in HTRAAPI_DATA_DIR). */
static const char default_htrausb_conf[] =
    "# Vital Product Data : Vendor/Device IDs - one per line.\n"
    "# Format - vendorID\tDeviceID\tFriendlyName (Max 30 chars or end of line)\n\n"
    "<VPD>\n"
    "367f    0001        HTRA\n"
    "6430    0001        SAE\n"
    "6430    0003        TRX\n"
    "6430    0005        SAM/SAN\n"
    "367f    0100        HTRA_VSG\n"
    "04b4    00f1        GENERICDEVICES\n"
    "</VPD>\n";

static char default_conf[PATH_MAX];
static pthread_once_t default_conf_once = PTHREAD_ONCE_INIT;

static void write_default_conf(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(default_conf, sizeof default_conf, "%s/htraapi-mac-%d-htrausb.conf",
             tmp && *tmp ? tmp : "/tmp", (int)getpid());
    FILE *f = fopen(default_conf, "w");
    if (!f || fwrite(default_htrausb_conf, 1, sizeof default_htrausb_conf - 1, f) !=
                  sizeof default_htrausb_conf - 1) {
        default_conf[0] = 0;
    }
    if (f) fclose(f);
}

/* Linux config paths, redirected in order to: the data directory, the real
 * Linux path, then the built-in default. */
static const char *redirect_path(const char *path, char *buf, size_t n) {
    if (path && !strcmp(path, "/etc/htrausb.conf")) {
        snprintf(buf, n, "%s/htrausb.conf", shim_data_dir());
        if (access(buf, R_OK) == 0) return buf;
        if (access(path, R_OK) == 0) return path;
        pthread_once(&default_conf_once, write_default_conf);
        if (default_conf[0]) return default_conf;
    }
    return path;
}

static FILE *lx_fopen(const char *path, const char *mode) {
    char buf[PATH_MAX];
    const char *real = redirect_path(path, buf, sizeof buf);
    FILE *f = fopen(real, mode);
    SHIM_TRACE("fopen(%s, %s) -> %s\n", real, mode, f ? "ok" : strerror(errno));
    if (!f) shim_fail();
    return f;
}

enum {
    LX_O_CREAT = 0x40, LX_O_EXCL = 0x80, LX_O_NOCTTY = 0x100, LX_O_TRUNC = 0x200,
    LX_O_APPEND = 0x400, LX_O_NONBLOCK = 0x800, LX_O_DIRECTORY = 0x4000,
    LX_O_NOFOLLOW = 0x8000, LX_O_CLOEXEC = 0x80000, LX_O_SYNC = 0x101000,
};

int shim_oflags_from_linux(int f) {
    int m = f & 3;
    if (f & LX_O_CREAT) m |= O_CREAT;
    if (f & LX_O_EXCL) m |= O_EXCL;
    if (f & LX_O_NOCTTY) m |= O_NOCTTY;
    if (f & LX_O_TRUNC) m |= O_TRUNC;
    if (f & LX_O_APPEND) m |= O_APPEND;
    if (f & LX_O_NONBLOCK) m |= O_NONBLOCK;
    if (f & LX_O_DIRECTORY) m |= O_DIRECTORY;
    if (f & LX_O_NOFOLLOW) m |= O_NOFOLLOW;
    if (f & LX_O_CLOEXEC) m |= O_CLOEXEC;
    if ((f & LX_O_SYNC) == LX_O_SYNC) m |= O_SYNC;
    return m;
}

int shim_oflags_to_linux(int m) {
    int f = m & 3;
    if (m & O_APPEND) f |= LX_O_APPEND;
    if (m & O_NONBLOCK) f |= LX_O_NONBLOCK;
    return f;
}

/* Linux passes the optional mode in x2 like a named argument. */
static int lx_open(const char *path, int flags, int mode) {
    char buf[PATH_MAX];
    const char *real = redirect_path(path, buf, sizeof buf);
    int fd = open(real, shim_oflags_from_linux(flags), mode);
    SHIM_TRACE("open(%s, 0x%x) -> %d\n", real, flags, fd);
    return fd < 0 ? (int)shim_fail() : fd;
}

static int lx_fcntl(int fd, int cmd, long arg) {
    int r;
    switch (cmd) {
    case 1: /* F_GETFD */
    case 2: /* F_SETFD: FD_CLOEXEC == 1 on both */
        r = fcntl(fd, cmd, (int)arg);
        break;
    case 3: /* F_GETFL */
        r = fcntl(fd, F_GETFL);
        if (r >= 0) r = shim_oflags_to_linux(r);
        break;
    case 4: /* F_SETFL */
        r = fcntl(fd, F_SETFL, shim_oflags_from_linux((int)arg));
        break;
    default:
        errno = EINVAL;
        r = -1;
    }
    return r < 0 ? (int)shim_fail() : r;
}

static int has_calfile_dir(const char *dir) {
    char p[PATH_MAX];
    struct stat st;
    snprintf(p, sizeof p, "%s/CalFile", dir);
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The SDK reads calibration from "<dir of /proc/self/exe>/CalFile/", and when
 * files are missing reads them from the analyzer's flash and caches them
 * there. Pick the directory, in order:
 *   1. $HTRAAPI_DATA_DIR
 *   2. the real executable's directory, if it has CalFile/ (native apps)
 *   3. the working directory, if it has CalFile/
 *   4. ~/Library/Application Support/htraapi (created; writable cache) */
static void calfile_root(char *out, size_t n) {
    if (getenv("HTRAAPI_DATA_DIR")) {
        strlcpy(out, shim_data_dir(), n);
        return;
    }
    char raw[PATH_MAX], exe[PATH_MAX];
    uint32_t sz = sizeof raw;
    if (!_NSGetExecutablePath(raw, &sz) && realpath(raw, exe)) {
        char *slash = strrchr(exe, '/');
        if (slash) *slash = 0;
        if (has_calfile_dir(exe)) {
            strlcpy(out, exe, n);
            return;
        }
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd) && has_calfile_dir(cwd)) {
        strlcpy(out, cwd, n);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/htraapi", home ? home : "/tmp");
    char cal[PATH_MAX];
    snprintf(cal, sizeof cal, "%s/CalFile", out);
    mkdir(out, 0755);
    mkdir(cal, 0755);
}

static ssize_t lx_readlink(const char *path, char *buf, size_t n) {
    if (path && !strcmp(path, "/proc/self/exe")) {
        char exe[PATH_MAX];
        calfile_root(exe, sizeof exe);
        strlcat(exe, "/htraapi", sizeof exe); /* the SDK strips the file name */
        size_t len = strlen(exe);
        if (len > n) len = n;
        memcpy(buf, exe, len); /* readlink does not NUL-terminate */
        SHIM_TRACE("readlink(/proc/self/exe) -> %.*s\n", (int)len, exe);
        return (ssize_t)len;
    }
    ssize_t r = readlink(path, buf, n);
    return r < 0 ? shim_fail() : r;
}

/* ---- Directories (struct dirent layout differs) ------------------------ */

struct lx_dirent {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
};

struct lx_dir {
    DIR *dir;
    struct lx_dirent ent;
};

static void *lx_opendir(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        shim_fail();
        return NULL;
    }
    struct lx_dir *w = calloc(1, sizeof *w);
    w->dir = d;
    return w;
}

static struct lx_dirent *lx_readdir(struct lx_dir *w) {
    struct dirent *e = readdir(w->dir);
    if (!e) return NULL;
    w->ent.d_ino = e->d_ino;
    w->ent.d_off = (int64_t)e->d_seekoff;
    w->ent.d_reclen = sizeof w->ent;
    w->ent.d_type = e->d_type; /* DT_* values match */
    strlcpy(w->ent.d_name, e->d_name, sizeof w->ent.d_name);
    return &w->ent;
}

static int lx_closedir(struct lx_dir *w) {
    int r = closedir(w->dir);
    free(w);
    return r;
}

/* ---- dl* --------------------------------------------------------------- */

static void *lx_dlopen(const char *name, int flags) {
    (void)flags;
    void *h = name ? shim_dlopen_elf(name) : NULL;
    SHIM_TRACE("dlopen(%s) -> %p\n", name ? name : "(null)", h);
    return h;
}
static void *lx_dlsym(void *h, const char *name) { return h ? shim_dlsym_elf(h, name) : NULL; }
static int lx_dlclose(void *h) {
    (void)h;
    return 0;
}

struct lx_dl_phdr_info {
    uint64_t dlpi_addr;
    const char *dlpi_name;
    const void *dlpi_phdr;
    uint16_t dlpi_phnum;
    unsigned long long dlpi_adds, dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
};

static int lx_dl_iterate_phdr(int (*cb)(struct lx_dl_phdr_info *, size_t, void *), void *data) {
    for (size_t i = 0; i < elf_count(); i++) {
        struct elf_object *o = elf_at(i);
        struct lx_dl_phdr_info info = {0};
        info.dlpi_addr = elf_base(o);
        info.dlpi_name = elf_name(o);
        info.dlpi_phdr = elf_phdrs(o, &info.dlpi_phnum);
        info.dlpi_adds = elf_count();
        int r = cb(&info, sizeof info, data);
        if (r) return r;
    }
    return 0;
}

/* ---- Misc -------------------------------------------------------------- */

static void lx_sincos(double x, double *s, double *c) {
    *s = sin(x);
    *c = cos(x);
}
static void lx_sincosf(float x, float *s, float *c) {
    *s = sinf(x);
    *c = cosf(x);
}

static int lx_omp_get_max_threads(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

/* glibc: SIG_BLOCK 0, SIG_UNBLOCK 1, SIG_SETMASK 2. sigset_t is larger on
 * Linux, but macOS only reads/writes the first 4 bytes. */
static int lx_pthread_sigmask(int how, const void *set, void *old) {
    static const int map[] = {SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK};
    if (how < 0 || how > 2) return EINVAL;
    return pthread_sigmask(map[how], set, old);
}

static int lx_sigfillset(void *set) {
    memset(set, 0xff, 128); /* sizeof(glibc sigset_t) */
    return 0;
}

extern FILE *__stdoutp, *__stderrp;

const struct shim_sym shim_libc_syms[] = {
    {"__errno_location", lx___errno_location},
    {"stdout", &__stdoutp},
    {"stderr", &__stderrp},
    {"read", lx_read},
    {"write", lx_write},
    {"close", lx_close},
    {"strerror_r", lx_strerror_r},
    {"__assert_fail", lx___assert_fail},
    {"printf", lx_printf},
    {"fprintf", lx_fprintf},
    {"snprintf", lx_snprintf},
    {"sscanf", lx_sscanf},
    {"__isoc99_sscanf", lx_sscanf},
    {"__isoc99_fscanf", lx_fscanf},
    {"__printf_chk", lx___printf_chk},
    {"__fprintf_chk", lx___fprintf_chk},
    {"__sprintf_chk", lx___sprintf_chk},
    {"__snprintf_chk", lx___snprintf_chk},
    {"__vsnprintf_chk", lx___vsnprintf_chk},
    {"__memcpy_chk", lx___memcpy_chk},
    {"__read_chk", lx___read_chk},
    {"__fread_chk", lx___fread_chk},
    {"vsnprintf", lx_vsnprintf},
    {"vfprintf", lx_vfprintf},
    {"__vfprintf_chk", lx___vfprintf_chk},
    {"fopen", lx_fopen},
    {"open", lx_open},
    {"fcntl", lx_fcntl},
    {"readlink", lx_readlink},
    {"opendir", lx_opendir},
    {"readdir", lx_readdir},
    {"closedir", lx_closedir},
    {"dlopen", lx_dlopen},
    {"dlsym", lx_dlsym},
    {"dlclose", lx_dlclose},
    {"dl_iterate_phdr", lx_dl_iterate_phdr},
    {"sincos", lx_sincos},
    {"sincosf", lx_sincosf},
    {"omp_get_max_threads", lx_omp_get_max_threads},
    {"pthread_sigmask", lx_pthread_sigmask},
    {"sigfillset", lx_sigfillset},
    {NULL, NULL},
};
