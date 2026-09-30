/*
 * stream_test: sustained IQS or RTA streaming at a chosen rate.
 *
 *   stream_test iqs|rta [seconds=60] [decimate=1]
 *
 * Prints a line every 5 s and a summary. For IQS it compares the delivered
 * sample rate with the configured IQ rate and checks the device timestamp of
 * every packet for gaps (dropped data). For RTA it reports the share of real
 * time captured (packets/s x acquisition time per packet). Exits non-zero on
 * any error, gap, or if the analyzer stops responding.
 *
 * Supply voltage and current (power and USB ports) are printed before and after.
 * Set HTRA_USB_POWER_ONLY=1 to open the analyzer as bus-powered.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "htra_api.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Supply voltage/current on the power port and USB port (as SAStudio shows). */
static void print_power(void **dev, const char *when) {
    PowerSupplyState_TypeDef ps;
    int st = Device_QueryPowerSupplyState(dev, &ps);
    if (st < 0) {
        printf("  power (%s): not available (%d)\n", when, st);
        return;
    }
    printf("  power (%s): power port %.2f V %.2f A (%.1f W), USB %.2f V %.2f A (%.1f W)\n", when,
           ps.rf_vlotage, ps.rf_current, ps.rf_vlotage * ps.rf_current, ps.usb_vlotage,
           ps.usb_current, ps.usb_vlotage * ps.usb_current);
}

/* Consecutive failed reads after which the analyzer is considered lost. */
#define LOST_AFTER 50

struct stats {
    long ok, errors, overflow, gaps;
    int consecutive_errors, last_error;
    double min_temp, max_temp;
};

static void note_temp(struct stats *s, int16_t t) {
    double c = t * 0.01;
    if (c < s->min_temp) s->min_temp = c;
    if (c > s->max_temp) s->max_temp = c;
}

/* Returns true to keep going. -12 (IF overflow) is a warning: data is valid. */
static bool note_status(struct stats *s, int st) {
    if (st == 0 || st == -12) {
        s->ok++;
        if (st == -12) s->overflow++;
        s->consecutive_errors = 0;
        return true;
    }
    s->errors++;
    s->last_error = st;
    return ++s->consecutive_errors < LOST_AFTER;
}

static int run_iqs(void **dev, double seconds, uint32_t decim) {
    IQS_Profile_TypeDef in, out;
    IQS_StreamInfo_TypeDef si;
    IQS_ProfileDeInit(dev, &in);
    in.CenterFreq_Hz = 100e6;
    in.RefLevel_dBm = -20;
    in.DecimateFactor = decim;
    in.DataFormat = Complex16bit;
    in.TriggerSource = Bus;
    in.TriggerMode = Adaptive;
    int st = IQS_Configuration(dev, &in, &out, &si);
    if (st < 0) {
        printf("IQS_Configuration failed: %d\n", st);
        return 1;
    }
    double rate = si.IQSampleRate, pkt_s = si.PacketSamples / rate;
    printf("IQS: %.3f MS/s (%.0f MB/s of 16-bit IQ), %u samples/packet\n", rate / 1e6,
           rate * 4 / 1e6, si.PacketSamples);

    struct stats s = {.min_temp = 1e9, .max_temp = -1e9};
    void *data = NULL;
    float scale;
    IQS_TriggerInfo_TypeDef trig;
    MeasAuxInfo_TypeDef aux;
    double last_ts = -1, t0 = now_s(), next_report = t0 + 5;
    long reported_ok = 0;
    IQS_BusTriggerStart(dev);
    while (now_s() - t0 < seconds) {
        st = IQS_GetIQStream(dev, &data, &scale, &trig, &aux);
        if (!note_status(&s, st)) break;
        if (st == 0 || st == -12) {
            note_temp(&s, aux.Temperature);
            /* Consecutive packets are pkt_s apart in device time; more is a gap. */
            if (last_ts >= 0 && aux.SysTimeStamp - last_ts > pkt_s * 1.5) s.gaps++;
            last_ts = aux.SysTimeStamp;
        }
        double t = now_s();
        if (t >= next_report) {
            double sps = (s.ok - reported_ok) * (double)si.PacketSamples / (t - next_report + 5);
            printf("  %5.0f s  %7.2f MS/s (%5.1f%%)  errors %ld  gaps %ld  IF overflow %ld  %.1f C\n",
                   t - t0, sps / 1e6, 100 * sps / rate, s.errors, s.gaps, s.overflow, s.max_temp);
            if (getenv("HTRA_POWER_DURING")) print_power(dev, "streaming");
            reported_ok = s.ok;
            next_report += 5;
        }
    }
    double dt = now_s() - t0;
    IQS_BusTriggerStop(dev);
    double sps = s.ok * (double)si.PacketSamples / dt;
    bool lost = s.consecutive_errors >= LOST_AFTER;
    printf("IQS summary: %.1f s, %ld packets, %.2f MS/s = %.1f%% of %.3f MS/s (%.0f MB/s), "
           "%ld errors%s, %ld gaps, %ld IF overflow, temperature %.1f-%.1f C%s\n",
           dt, s.ok, sps / 1e6, 100 * sps / rate, rate / 1e6, sps * 4 / 1e6, s.errors,
           s.errors ? " (last status)" : "", s.gaps, s.overflow, s.min_temp, s.max_temp,
           lost ? "  ** ANALYZER LOST **" : "");
    if (s.errors) printf("  last error status: %d\n", s.last_error);
    return (s.errors || s.gaps || lost) ? 1 : 0;
}

static int run_rta(void **dev, double seconds, uint32_t decim) {
    RTA_Profile_TypeDef in, out;
    RTA_FrameInfo_TypeDef fi;
    RTA_ProfileDeInit(dev, &in);
    in.CenterFreq_Hz = 100e6;
    in.RefLevel_dBm = -20;
    in.DecimateFactor = decim;
    in.TriggerMode = Adaptive;
    in.TriggerSource = Bus;
    int st = RTA_Configuration(dev, &in, &out, &fi);
    if (st < 0) {
        printf("RTA_Configuration failed: %d\n", st);
        return 1;
    }
    printf("RTA: %.3f-%.3f MHz, %u frames/packet, %.3f ms per packet, %u samples/packet, POI %.2f us\n",
           fi.StartFrequency_Hz / 1e6, fi.StopFrequency_Hz / 1e6, fi.PacketFrame,
           fi.PacketAcqTime * 1e3, fi.PacketSamplePoints, fi.POI * 1e6);

    uint8_t *trace = malloc(fi.PacketValidPoints);
    uint16_t *bitmap = malloc((size_t)fi.FrameHeight * fi.FrameWidth * sizeof *bitmap);
    RTA_PlotInfo_TypeDef plot;
    RTA_TriggerInfo_TypeDef trig;
    MeasAuxInfo_TypeDef aux;
    struct stats s = {.min_temp = 1e9, .max_temp = -1e9};
    double t0 = now_s(), next_report = t0 + 5;
    long reported_ok = 0;
    RTA_BusTriggerStart(dev);
    while (now_s() - t0 < seconds) {
        st = RTA_GetRealTimeSpectrum(dev, trace, bitmap, &plot, &trig, &aux);
        if (!note_status(&s, st)) break;
        if (st == 0 || st == -12) note_temp(&s, aux.Temperature);
        double t = now_s();
        if (t >= next_report) {
            double pps = (s.ok - reported_ok) / (t - next_report + 5);
            printf("  %5.0f s  %8.1f packets/s  real-time %5.1f%%  errors %ld  IF overflow %ld  %.1f C\n",
                   t - t0, pps, 100 * pps * fi.PacketAcqTime, s.errors, s.overflow, s.max_temp);
            if (getenv("HTRA_POWER_DURING")) print_power(dev, "streaming");
            reported_ok = s.ok;
            next_report += 5;
        }
    }
    double dt = now_s() - t0;
    RTA_BusTriggerStop(dev);
    double pps = s.ok / dt;
    bool lost = s.consecutive_errors >= LOST_AFTER;
    printf("RTA summary: %.1f s, %ld packets (%.1f/s), real-time coverage %.1f%%, %ld errors, "
           "%ld IF overflow, temperature %.1f-%.1f C%s\n",
           dt, s.ok, pps, 100 * pps * fi.PacketAcqTime, s.errors, s.overflow, s.min_temp,
           s.max_temp, lost ? "  ** ANALYZER LOST **" : "");
    if (s.errors) printf("  last error status: %d\n", s.last_error);
    free(trace);
    free(bitmap);
    return (s.errors || lost) ? 1 : 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2 || (strcmp(argv[1], "iqs") && strcmp(argv[1], "rta"))) {
        fprintf(stderr, "usage: %s iqs|rta [seconds=60] [decimate=1]\n", argv[0]);
        return 2;
    }
    double seconds = argc > 2 ? atof(argv[2]) : 60;
    uint32_t decim = argc > 3 ? (uint32_t)atoi(argv[3]) : 1;

    void *dev = NULL;
    BootProfile_TypeDef boot;
    BootInfo_TypeDef info;
    memset(&boot, 0, sizeof boot);
    boot.PhysicalInterface = USB;
    boot.DevicePowerSupply = getenv("HTRA_USB_POWER_ONLY") ? USBPortOnly : USBPortAndPowerPort;
    int st = Device_Open(&dev, 0, &boot, &info);
    printf("Device_Open: %d (model 0x%x, firmware 0x%x, bus speed %d)\n", st,
           info.DeviceInfo.Model, info.DeviceInfo.MFWVersion, info.BusSpeed);
    if (st < 0) return 1;
    print_power(&dev, "idle");
    int rc = !strcmp(argv[1], "iqs") ? run_iqs(&dev, seconds, decim) : run_rta(&dev, seconds, decim);
    print_power(&dev, "after streaming");
    printf("Device_Close: %d\n", Device_Close(&dev));
    printf("%s\n", rc ? "FAILED" : "PASSED");
    return rc;
}
