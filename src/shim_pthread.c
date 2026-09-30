/*
 * pthread shims. glibc (aarch64) and macOS disagree on object sizes and
 * static initializers:
 *
 *   type                glibc   macOS
 *   pthread_mutex_t       48      64   (glibc initializer is all zero)
 *   pthread_cond_t        48      48   (macOS initializer is non-zero)
 *   pthread_condattr_t     8      16
 *   pthread_once_t         4      16
 *   pthread_key_t          4       8
 *
 * Linux-side mutex/cond objects therefore hold a pointer (first 8 bytes) to a
 * lazily created native object. glibc zero-initialized objects map to NULL.
 */
#include "shim.h"

#include <errno.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { LX_CLOCK_REALTIME = 0, LX_CLOCK_MONOTONIC = 1, LX_ETIMEDOUT = 110 };

struct native_cond {
    pthread_cond_t cond;
    int clock;
};

pthread_mutex_t *shim_mutex(void *m) {
    _Atomic(pthread_mutex_t *) *slot = m;
    pthread_mutex_t *p = atomic_load_explicit(slot, memory_order_acquire);
    if (p) return p;
    pthread_mutex_t *n = malloc(sizeof *n);
    pthread_mutex_init(n, NULL);
    if (atomic_compare_exchange_strong(slot, &p, n)) return n;
    pthread_mutex_destroy(n);
    free(n);
    return p;
}

static struct native_cond *cond_create(int clock) {
    struct native_cond *c = malloc(sizeof *c);
    pthread_cond_init(&c->cond, NULL);
    c->clock = clock;
    return c;
}

struct native_cond *shim_cond(void *cv) {
    _Atomic(struct native_cond *) *slot = cv;
    struct native_cond *p = atomic_load_explicit(slot, memory_order_acquire);
    if (p) return p;
    struct native_cond *n = cond_create(LX_CLOCK_REALTIME);
    if (atomic_compare_exchange_strong(slot, &p, n)) return n;
    pthread_cond_destroy(&n->cond);
    free(n);
    return p;
}

void shim_cond_free(void *cv) {
    _Atomic(struct native_cond *) *slot = cv;
    struct native_cond *p = atomic_exchange(slot, NULL);
    if (p) {
        pthread_cond_destroy(&p->cond);
        free(p);
    }
}

/* ---- mutex ------------------------------------------------------------- */

static int lx_pthread_mutex_init(void *m, const void *attr) {
    (void)attr; /* only default attributes are used by the SDK */
    memset(m, 0, 48);
    shim_mutex(m);
    return 0;
}
static int lx_pthread_mutex_lock(void *m) { return pthread_mutex_lock(shim_mutex(m)); }
static int lx_pthread_mutex_unlock(void *m) { return pthread_mutex_unlock(shim_mutex(m)); }
static int lx_pthread_mutex_trylock(void *m) { return pthread_mutex_trylock(shim_mutex(m)); }
static int lx_pthread_mutex_destroy(void *m) {
    _Atomic(pthread_mutex_t *) *slot = m;
    pthread_mutex_t *p = atomic_exchange(slot, NULL);
    if (p) {
        pthread_mutex_destroy(p);
        free(p);
    }
    return 0;
}

/* ---- condition variables ----------------------------------------------- */

static int lx_pthread_condattr_init(int *attr) {
    *attr = LX_CLOCK_REALTIME;
    return 0;
}
static int lx_pthread_condattr_destroy(int *attr) {
    (void)attr;
    return 0;
}
static int lx_pthread_condattr_setclock(int *attr, int clock) {
    if (clock != LX_CLOCK_REALTIME && clock != LX_CLOCK_MONOTONIC) return EINVAL;
    *attr = clock;
    return 0;
}

static int lx_pthread_cond_init(void *cv, const int *attr) {
    _Atomic(struct native_cond *) *slot = cv;
    memset(cv, 0, 48);
    atomic_store(slot, cond_create(attr ? *attr : LX_CLOCK_REALTIME));
    return 0;
}
static int lx_pthread_cond_destroy(void *cv) {
    shim_cond_free(cv);
    return 0;
}
static int lx_pthread_cond_signal(void *cv) { return pthread_cond_signal(&shim_cond(cv)->cond); }
static int lx_pthread_cond_broadcast(void *cv) { return pthread_cond_broadcast(&shim_cond(cv)->cond); }
static int lx_pthread_cond_wait(void *cv, void *m) {
    return pthread_cond_wait(&shim_cond(cv)->cond, shim_mutex(m));
}

static int lx_pthread_cond_timedwait(void *cv, void *m, const struct timespec *abstime) {
    struct native_cond *c = shim_cond(cv);
    int r;
    if (c->clock == LX_CLOCK_MONOTONIC) {
        struct timespec now, rel;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long ns = (long long)(abstime->tv_sec - now.tv_sec) * 1000000000LL +
                       (abstime->tv_nsec - now.tv_nsec);
        if (ns < 0) ns = 0;
        rel.tv_sec = ns / 1000000000LL;
        rel.tv_nsec = ns % 1000000000LL;
        r = pthread_cond_timedwait_relative_np(&c->cond, shim_mutex(m), &rel);
    } else {
        r = pthread_cond_timedwait(&c->cond, shim_mutex(m), abstime);
    }
    return r == ETIMEDOUT ? LX_ETIMEDOUT : r;
}

/* ---- once -------------------------------------------------------------- */

enum { ONCE_INIT = 0, ONCE_RUNNING = 1, ONCE_DONE = 2 };

static void once_reset_on_unwind(_Atomic uint32_t **p) {
    /* Reached only if init_routine threw: allow another attempt. */
    if (*p) {
        atomic_store(*p, ONCE_INIT);
        os_sync_wake_by_address_all(*p, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    }
}

static int lx_pthread_once(_Atomic uint32_t *once, void (*init)(void)) {
    for (;;) {
        uint32_t s = atomic_load_explicit(once, memory_order_acquire);
        if (s == ONCE_DONE) return 0;
        if (s == ONCE_INIT && atomic_compare_exchange_strong(once, &s, ONCE_RUNNING)) {
            _Atomic uint32_t *guard __attribute__((cleanup(once_reset_on_unwind))) = once;
            init();
            guard = NULL;
            atomic_store_explicit(once, ONCE_DONE, memory_order_release);
            os_sync_wake_by_address_all(once, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
            return 0;
        }
        if (s == ONCE_RUNNING)
            os_sync_wait_on_address(once, ONCE_RUNNING, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
    }
}

/* ---- thread-specific data ---------------------------------------------- */

static int lx_pthread_key_create(uint32_t *key, void (*dtor)(void *)) {
    pthread_key_t k;
    int r = pthread_key_create(&k, dtor);
    if (!r) *key = (uint32_t)k;
    return r;
}
static int lx_pthread_key_delete(uint32_t key) { return pthread_key_delete(key); }
static void *lx_pthread_getspecific(uint32_t key) { return pthread_getspecific(key); }
static int lx_pthread_setspecific(uint32_t key, const void *v) { return pthread_setspecific(key, v); }

/* ---- threads ----------------------------------------------------------- */

static int lx_pthread_create(pthread_t *t, const void *attr, void *(*fn)(void *), void *arg) {
    (void)attr; /* the SDK never customizes thread attributes */
    return pthread_create(t, NULL, fn, arg);
}

const struct shim_sym shim_pthread_syms[] = {
    {"pthread_mutex_init", lx_pthread_mutex_init},
    {"pthread_mutex_lock", lx_pthread_mutex_lock},
    {"pthread_mutex_unlock", lx_pthread_mutex_unlock},
    {"pthread_mutex_trylock", lx_pthread_mutex_trylock},
    {"pthread_mutex_destroy", lx_pthread_mutex_destroy},
    {"pthread_condattr_init", lx_pthread_condattr_init},
    {"pthread_condattr_destroy", lx_pthread_condattr_destroy},
    {"pthread_condattr_setclock", lx_pthread_condattr_setclock},
    {"pthread_cond_init", lx_pthread_cond_init},
    {"pthread_cond_destroy", lx_pthread_cond_destroy},
    {"pthread_cond_signal", lx_pthread_cond_signal},
    {"pthread_cond_broadcast", lx_pthread_cond_broadcast},
    {"pthread_cond_wait", lx_pthread_cond_wait},
    {"pthread_cond_timedwait", lx_pthread_cond_timedwait},
    {"pthread_once", lx_pthread_once},
    {"pthread_key_create", lx_pthread_key_create},
    {"__pthread_key_create", lx_pthread_key_create},
    {"pthread_key_delete", lx_pthread_key_delete},
    {"pthread_getspecific", lx_pthread_getspecific},
    {"pthread_setspecific", lx_pthread_setspecific},
    {"pthread_create", lx_pthread_create},
    {NULL, NULL},
};
