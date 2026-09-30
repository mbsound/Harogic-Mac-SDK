/*
 * libstdc++ bridging. The vendor code was built against GNU/Linux libstdc++;
 * we run it against Homebrew GCC's libstdc++ for Darwin, which shares the
 * Itanium C++ ABI and std::__cxx11 layouts. Only these pieces differ:
 *
 *  - std::condition_variable embeds a pthread_cond_t and is used with
 *    std::mutex objects laid out by the vendor code (see shim_pthread.c).
 *  - std::__atomic_futex_unsigned_base exists only where Linux futexes do.
 *  - std::ctype<char>'s classification table has a different mask encoding
 *    (unsigned short, glibc bits) and is NULL on Darwin, yet the vendor code
 *    inlines lookups into it (std::regex).
 */
#include "shim.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct native_cond;
pthread_mutex_t *shim_mutex(void *m);
struct native_cond *shim_cond(void *cv);
void shim_cond_free(void *cv);
/* The native cond is the first member of struct native_cond. */
#define NATIVE_COND(cv) ((pthread_cond_t *)shim_cond(cv))

/* ---- std::condition_variable (48-byte glibc layout) -------------------- */

static void cv_ctor(void *self) { memset(self, 0, 48); }
static void cv_dtor(void *self) { shim_cond_free(self); }
static void cv_notify_one(void *self) { pthread_cond_signal(NATIVE_COND(self)); }
static void cv_notify_all(void *self) { pthread_cond_broadcast(NATIVE_COND(self)); }

struct unique_lock { void *mutex; bool owns; };

static void cv_wait(void *self, struct unique_lock *lk) {
    pthread_cond_wait(NATIVE_COND(self), shim_mutex(lk->mutex));
}

/* ---- std::__atomic_futex_unsigned_base --------------------------------- */

/* Blocks while *addr == val. Absolute CLOCK_REALTIME deadline if has_timeout.
 * Returns false only on timeout. */
static bool futex_wait_until(void *self, unsigned *addr, unsigned val, bool has_timeout,
                             long s, long ns) {
    (void)self;
    if (!has_timeout) {
        os_sync_wait_on_address(addr, val, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
        return true; /* callers re-check the value */
    }
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    long long rel = (long long)(s - now.tv_sec) * 1000000000LL + (ns - now.tv_nsec);
    if (rel <= 0) return false;
    if (os_sync_wait_on_address_with_timeout(addr, val, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                             OS_CLOCK_MACH_ABSOLUTE_TIME, (uint64_t)rel) < 0 &&
        errno == ETIMEDOUT)
        return false;
    return true;
}

static void futex_notify_all(unsigned *addr) {
    os_sync_wake_by_address_all(addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

/* ---- std::use_facet<std::ctype<char>> ---------------------------------- */

/*
 * Layout shared by both libstdc++ builds (bits/locale_facets.h):
 *   0 vptr, 8 refcount, 16 _M_c_locale_ctype, 24 _M_del, 32 _M_toupper,
 *   40 _M_tolower, 48 _M_table, 56 _M_widen_ok, 57 _M_widen[256],
 *   313 _M_narrow[256], 569 _M_narrow_ok  => sizeof 576.
 * We hand the vendor code a copy of the native facet whose _M_table points to
 * a glibc-format table. Virtual calls still dispatch to native code.
 */
#define CTYPE_SIZE 576
#define CTYPE_TABLE_OFF 48

enum { /* glibc _ISbit() values, little-endian */
    G_UPPER = 0x100, G_LOWER = 0x200, G_ALPHA = 0x400, G_DIGIT = 0x800,
    G_XDIGIT = 0x1000, G_SPACE = 0x2000, G_PRINT = 0x4000, G_GRAPH = 0x8000,
    G_BLANK = 0x1, G_CNTRL = 0x2, G_PUNCT = 0x4, G_ALNUM = 0x8,
};

static uint16_t gnu_table[384];

static void build_gnu_table(void) {
    for (int c = 0; c < 256; c++) {
        uint16_t m = 0;
        if (c < 128) { /* "C" locale classification */
            if (isupper(c)) m |= G_UPPER;
            if (islower(c)) m |= G_LOWER;
            if (isalpha(c)) m |= G_ALPHA;
            if (isdigit(c)) m |= G_DIGIT;
            if (isxdigit(c)) m |= G_XDIGIT;
            if (isspace(c)) m |= G_SPACE;
            if (isprint(c)) m |= G_PRINT;
            if (isgraph(c)) m |= G_GRAPH;
            if (isblank(c)) m |= G_BLANK;
            if (iscntrl(c)) m |= G_CNTRL;
            if (ispunct(c)) m |= G_PUNCT;
            if (isalnum(c)) m |= G_ALNUM;
        }
        gnu_table[128 + c] = m;
        if (c >= 128) gnu_table[c - 128] = m; /* signed-char indices */
    }
}

static const void *(*native_use_facet_ctype)(const void *locale);
static pthread_mutex_t facet_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { const void *native; void *shadow; } facet_cache[16];

static const void *use_facet_ctype(const void *locale) {
    const void *native = native_use_facet_ctype(locale);
    pthread_mutex_lock(&facet_lock);
    void *shadow = NULL;
    for (int i = 0; i < 16 && facet_cache[i].native; i++)
        if (facet_cache[i].native == native) shadow = facet_cache[i].shadow;
    if (!shadow) {
        shadow = malloc(CTYPE_SIZE);
        memcpy(shadow, native, CTYPE_SIZE);
        const uint16_t *tbl = &gnu_table[128];
        memcpy((char *)shadow + CTYPE_TABLE_OFF, &tbl, sizeof tbl);
        for (int i = 0; i < 16; i++)
            if (!facet_cache[i].native) {
                facet_cache[i].native = native;
                facet_cache[i].shadow = shadow;
                break;
            }
    }
    pthread_mutex_unlock(&facet_lock);
    return shadow;
}

void shim_cxx_init(void *libstdcxx) {
    build_gnu_table();
    native_use_facet_ctype = dlsym(libstdcxx, "_ZSt9use_facetISt5ctypeIcEERKT_RKSt6locale");
}

const struct shim_sym shim_cxx_syms[] = {
    {"_ZNSt18condition_variableC1Ev", cv_ctor},
    {"_ZNSt18condition_variableC2Ev", cv_ctor},
    {"_ZNSt18condition_variableD1Ev", cv_dtor},
    {"_ZNSt18condition_variableD2Ev", cv_dtor},
    {"_ZNSt18condition_variable10notify_oneEv", cv_notify_one},
    {"_ZNSt18condition_variable10notify_allEv", cv_notify_all},
    {"_ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE", cv_wait},
    {"_ZNSt28__atomic_futex_unsigned_base19_M_futex_wait_untilEPjjbNSt6chrono8durationIlSt5ratioILl1ELl1EEEENS2_IlS3_ILl1ELl1000000000EEEE",
     futex_wait_until},
    {"_ZNSt28__atomic_futex_unsigned_base19_M_futex_notify_allEPj", futex_notify_all},
    {"_ZSt9use_facetISt5ctypeIcEERKT_RKSt6locale", use_facet_ctype},
    {NULL, NULL},
};
