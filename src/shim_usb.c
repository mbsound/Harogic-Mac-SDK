/*
 * libusb bridging.
 *
 * 1. ABI: Apple's ABI requires callers to zero-extend 8/16-bit arguments;
 *    AAPCS64 leaves the upper bits undefined. Functions taking such arguments
 *    are wrapped to mask them. Everything else resolves directly to
 *    Homebrew's libusb.
 *
 * 2. Streaming read-ahead. The SDK streams IQ/RTA data with back-to-back
 *    synchronous 64 KB bulk reads and nothing queued in between. On Linux the
 *    turnaround is short enough; on macOS it is ~50 us median with multi-
 *    hundred-microsecond spikes, and at the top rate (250 MB/s) the analyzer's
 *    FIFO overflows and its firmware drops off the bus. When a thread issues
 *    several consecutive full reads of the same size on an IN endpoint, we keep
 *    a queue of asynchronous reads posted and serve the synchronous calls from
 *    it, in order. Any other transfer from that thread, or a different read
 *    size, ends read-ahead; completed-but-unread buffers are kept and served
 *    first so no data is lost.
 *
 *    HTRAAPI_USB_READAHEAD=<n> sets the queue depth (default 16, 0 disables);
 *    HTRAAPI_USB_READAHEAD_STREAK=<n> the reads needed to engage (default 2).
 */
#include "shim.h"

#include <libusb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* ---- context capture --------------------------------------------------- */

static libusb_context *g_ctx; /* the SDK's context (NULL = default context) */

static int w_init(libusb_context **ctx) {
    int r = libusb_init(ctx);
    if (r == 0 && ctx) g_ctx = *ctx;
    return r;
}

/* ---- read-ahead -------------------------------------------------------- */

#define RA_MAX_DEPTH 64
#define RA_MIN_LEN 32768
static int ra_streak(void) {
    const char *e = getenv("HTRAAPI_USB_READAHEAD_STREAK");
    return e && atoi(e) > 0 ? atoi(e) : 2;
}

struct ra_slot {
    struct libusb_transfer *xfer;
    unsigned char *buf;
    int done;      /* set by the completion callback */
    int submitted; /* in flight or completed, not yet consumed */
};

struct readahead {
    libusb_device_handle *h;
    uint8_t ep;
    int len;
    pthread_t owner;   /* thread that issues the stream reads */
    int streak;        /* consecutive full reads of `len` */
    int active;        /* resubmitting */
    int depth;
    int head, count;   /* ring of submitted slots, in submission order */
    struct ra_slot slot[RA_MAX_DEPTH];
};

static struct readahead g_ra; /* the SDK streams one endpoint at a time */
static int g_ra_depth = -1;

static int ra_depth(void) {
    if (g_ra_depth < 0) {
        const char *e = getenv("HTRAAPI_USB_READAHEAD");
        g_ra_depth = e ? atoi(e) : 16;
        if (g_ra_depth > RA_MAX_DEPTH) g_ra_depth = RA_MAX_DEPTH;
        if (g_ra_depth < 0) g_ra_depth = 0;
    }
    return g_ra_depth;
}

static void LIBUSB_CALL ra_callback(struct libusb_transfer *t) {
    ((struct ra_slot *)t->user_data)->done = 1;
}

static int ra_submit(struct ra_slot *s) {
    s->done = 0;
    libusb_fill_bulk_transfer(s->xfer, g_ra.h, g_ra.ep, s->buf, g_ra.len, ra_callback, s, 0);
    int r = libusb_submit_transfer(s->xfer);
    s->submitted = r == 0;
    return r;
}

/* Wait for a slot to complete, up to timeout_ms (0 = forever). */
static int ra_wait(struct ra_slot *s, unsigned timeout_ms) {
    struct timeval start, now;
    gettimeofday(&start, NULL);
    while (!s->done) {
        struct timeval tv = {0, 100000};
        if (timeout_ms) {
            gettimeofday(&now, NULL);
            long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
            if (elapsed >= (long)timeout_ms) return 0;
            long left = timeout_ms - elapsed;
            if (left < 100) tv.tv_usec = left * 1000;
        }
        libusb_handle_events_timeout_completed(g_ctx, &tv, &s->done);
    }
    return 1;
}

static int transfer_status_to_error(enum libusb_transfer_status st) {
    switch (st) {
    case LIBUSB_TRANSFER_COMPLETED: return 0;
    case LIBUSB_TRANSFER_TIMED_OUT: return LIBUSB_ERROR_TIMEOUT;
    case LIBUSB_TRANSFER_STALL: return LIBUSB_ERROR_PIPE;
    case LIBUSB_TRANSFER_NO_DEVICE: return LIBUSB_ERROR_NO_DEVICE;
    case LIBUSB_TRANSFER_OVERFLOW: return LIBUSB_ERROR_OVERFLOW;
    default: return LIBUSB_ERROR_IO;
    }
}

/* Stop resubmitting and cancel reads that have not received data. Buffers that
 * already completed stay queued and are served to subsequent reads. */
static void ra_stop(void) {
    if (!g_ra.count && !g_ra.active) return;
    g_ra.active = 0;
    for (int i = 0; i < g_ra.count; i++) {
        struct ra_slot *s = &g_ra.slot[(g_ra.head + i) % g_ra.depth];
        if (!s->done) libusb_cancel_transfer(s->xfer);
    }
    /* Reap cancellations; keep only a prefix of full, successful buffers. */
    int keep = 0, contiguous = 1;
    for (int i = 0; i < g_ra.count; i++) {
        struct ra_slot *s = &g_ra.slot[(g_ra.head + i) % g_ra.depth];
        ra_wait(s, 0);
        int full = s->xfer->status == LIBUSB_TRANSFER_COMPLETED;
        if (contiguous && full) keep++;
        else contiguous = 0;
        if (!contiguous) s->submitted = 0;
    }
    SHIM_TRACE("usb read-ahead stopped on ep 0x%02x (%d buffered)\n", g_ra.ep, keep);
    g_ra.count = keep;
    g_ra.streak = 0;
}

/* A command from the streaming thread: the first one stops read-ahead and
 * keeps completed buffers (a query in the middle of a stream); a second one
 * before the stream resumes means it was stopped or reconfigured, so the
 * buffered data is stale. */
static void ra_on_command(void) {
    if (!pthread_equal(g_ra.owner, pthread_self())) return;
    if (g_ra.active) ra_stop();
    else if (g_ra.count) {
        SHIM_TRACE("usb read-ahead: discarding %d stale buffers\n", g_ra.count);
        g_ra.count = 0;
    }
}

static void ra_release(void) {
    ra_stop();
    for (int i = 0; i < RA_MAX_DEPTH; i++) {
        if (g_ra.slot[i].xfer) libusb_free_transfer(g_ra.slot[i].xfer);
        free(g_ra.slot[i].buf);
    }
    memset(&g_ra, 0, sizeof g_ra);
}

static int ra_start(libusb_device_handle *h, uint8_t ep, int len) {
    if (g_ra.len != len || g_ra.h != h || g_ra.ep != ep) {
        ra_release();
        g_ra.h = h;
        g_ra.ep = ep;
        g_ra.len = len;
    }
    g_ra.depth = ra_depth();
    for (int i = 0; i < g_ra.depth; i++) {
        struct ra_slot *s = &g_ra.slot[i];
        if (!s->xfer) s->xfer = libusb_alloc_transfer(0);
        if (!s->buf) s->buf = malloc((size_t)len);
        if (!s->xfer || !s->buf) return -1;
    }
    g_ra.active = 1;
    while (g_ra.count < g_ra.depth) {
        struct ra_slot *s = &g_ra.slot[(g_ra.head + g_ra.count) % g_ra.depth];
        if (ra_submit(s)) break;
        g_ra.count++;
    }
    SHIM_TRACE("usb read-ahead started on ep 0x%02x: %d x %d bytes\n", ep, g_ra.count, len);
    return 0;
}

/* Serve one read from the queue. Returns 1 if handled. */
static int ra_read(libusb_device_handle *h, uint8_t ep, unsigned char *data, int len,
                   int *transferred, unsigned timeout, int *result) {
    if (!g_ra.count || g_ra.h != h || g_ra.ep != ep) return 0;
    if (len != g_ra.len) { /* stream reconfigured: drop stale buffers */
        ra_stop();
        g_ra.count = 0;
        return 0;
    }
    struct ra_slot *s = &g_ra.slot[g_ra.head];
    if (!ra_wait(s, timeout)) {
        if (transferred) *transferred = 0;
        *result = LIBUSB_ERROR_TIMEOUT; /* stays queued; served next call */
        return 1;
    }
    int n = s->xfer->actual_length;
    memcpy(data, s->buf, (size_t)n);
    if (transferred) *transferred = n;
    *result = transfer_status_to_error(s->xfer->status);
    s->submitted = 0;
    g_ra.head = (g_ra.head + 1) % g_ra.depth;
    g_ra.count--;
    if (*result) {
        ra_stop();
        g_ra.count = 0;
    } else if (g_ra.active) {
        struct ra_slot *t = &g_ra.slot[(g_ra.head + g_ra.count) % g_ra.depth];
        if (ra_submit(t) == 0) g_ra.count++;
    }
    return 1;
}

static int w_bulk_transfer(libusb_device_handle *h, uint64_t ep64, unsigned char *data, int len,
                           int *transferred, unsigned int timeout) {
    uint8_t ep = (uint8_t)ep64;
    int is_in = ep & LIBUSB_ENDPOINT_IN;
    int depth = ra_depth();

    if (depth && is_in) {
        int r;
        if (ra_read(h, ep, data, len, transferred, timeout, &r)) return r;
    } else if (depth) {
        ra_on_command();
    }

    int got = 0;
    int r = libusb_bulk_transfer(h, ep, data, len, &got, timeout);
    if (transferred) *transferred = got;

    if (depth && is_in) {
        if (r == 0 && got == len && len >= RA_MIN_LEN && g_ra.h == h && g_ra.ep == ep &&
            g_ra.len == len && pthread_equal(g_ra.owner, pthread_self())) {
            if (++g_ra.streak >= ra_streak()) ra_start(h, ep, len);
        } else if (r == 0 && got == len && len >= RA_MIN_LEN) {
            if (g_ra.h != h || g_ra.ep != ep || g_ra.len != len) ra_release();
            g_ra.h = h;
            g_ra.ep = ep;
            g_ra.len = len;
            g_ra.owner = pthread_self();
            g_ra.streak = 1;
            if (ra_streak() <= 1) ra_start(h, ep, len);
        } else {
            g_ra.streak = 0;
        }
    }
    return r;
}

static int w_control_transfer(libusb_device_handle *h, uint64_t type, uint64_t req,
                              uint64_t value, uint64_t index, unsigned char *data,
                              uint64_t len, unsigned int timeout) {
    ra_on_command();
    return libusb_control_transfer(h, (uint8_t)type, (uint8_t)req, (uint16_t)value,
                                   (uint16_t)index, data, (uint16_t)len, timeout);
}

static int w_release_interface(libusb_device_handle *h, int iface) {
    if (g_ra.h == h) ra_release();
    return libusb_release_interface(h, iface);
}

static void w_close(libusb_device_handle *h) {
    if (h && g_ra.h == h) ra_release();
    libusb_close(h);
}

/* ---- zero-extension wrappers ------------------------------------------- */

static int w_interrupt_transfer(libusb_device_handle *h, uint64_t ep, unsigned char *data,
                                int len, int *transferred, unsigned int timeout) {
    return libusb_interrupt_transfer(h, (uint8_t)ep, data, len, transferred, timeout);
}

static int w_clear_halt(libusb_device_handle *h, uint64_t ep) {
    return libusb_clear_halt(h, (uint8_t)ep);
}

static int w_get_max_packet_size(libusb_device *d, uint64_t ep) {
    return libusb_get_max_packet_size(d, (uint8_t)ep);
}

static int w_get_max_iso_packet_size(libusb_device *d, uint64_t ep) {
    return libusb_get_max_iso_packet_size(d, (uint8_t)ep);
}

static int w_get_string_descriptor_ascii(libusb_device_handle *h, uint64_t idx,
                                         unsigned char *data, int len) {
    return libusb_get_string_descriptor_ascii(h, (uint8_t)idx, data, len);
}

static int w_get_config_descriptor(libusb_device *d, uint64_t idx,
                                   struct libusb_config_descriptor **cfg) {
    return libusb_get_config_descriptor(d, (uint8_t)idx, cfg);
}

static int w_get_config_descriptor_by_value(libusb_device *d, uint64_t v,
                                            struct libusb_config_descriptor **cfg) {
    return libusb_get_config_descriptor_by_value(d, (uint8_t)v, cfg);
}

const struct shim_sym shim_usb_syms[] = {
    {"libusb_init", w_init},
    {"libusb_bulk_transfer", w_bulk_transfer},
    {"libusb_control_transfer", w_control_transfer},
    {"libusb_release_interface", w_release_interface},
    {"libusb_close", w_close},
    {"libusb_interrupt_transfer", w_interrupt_transfer},
    {"libusb_clear_halt", w_clear_halt},
    {"libusb_get_max_packet_size", w_get_max_packet_size},
    {"libusb_get_max_iso_packet_size", w_get_max_iso_packet_size},
    {"libusb_get_string_descriptor_ascii", w_get_string_descriptor_ascii},
    {"libusb_get_config_descriptor", w_get_config_descriptor},
    {"libusb_get_config_descriptor_by_value", w_get_config_descriptor_by_value},
    {NULL, NULL},
};
