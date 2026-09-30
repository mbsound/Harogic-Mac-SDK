/*
 * Exercises every measurement mode fReqon uses (plus MSCAN) against a real
 * analyzer, and runs an SWP stress loop.
 *
 *   ./build/mode_test [modes...]   modes: state rta iqs det mscan swp (default: all)
 *   HTRA_SWEEPS=n sets the SWP stress count.
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

static int failures;
#define CHECK(label, st)                                                       \
    do {                                                                       \
        int s_ = (st);                                                         \
        printf("  %-28s %s (%d)\n", label, s_ >= 0 ? "ok" : "FAIL", s_);       \
        if (s_ < 0) failures++;                                                \
    } while (0)

static void test_state(void **dev) {
    printf("[device state]\n");
    DeviceState_TypeDef ds;
    CHECK("Device_QueryDeviceState", Device_QueryDeviceState(dev, &ds));
    printf("  temperature %d, RF state 0x%x, sample rate %u\n", ds.Temperature, ds.RFState,
           ds.SampleRate);
}

static void test_rta(void **dev) {
    printf("[RTA]\n");
    RTA_Profile_TypeDef in, out;
    RTA_FrameInfo_TypeDef fi;
    RTA_ProfileDeInit(dev, &in);
    in.CenterFreq_Hz = 100e6;
    in.RefLevel_dBm = -20;
    in.DecimateFactor = getenv("HTRA_DECIM") ? atoi(getenv("HTRA_DECIM")) : 1;
    in.TriggerMode = Adaptive;
    in.TriggerSource = Bus;
    int st = RTA_Configuration(dev, &in, &out, &fi);
    CHECK("RTA_Configuration", st);
    if (st < 0) return;
    printf("  %.3f-%.3f MHz, %u x %u bitmap, %u packets\n", fi.StartFrequency_Hz / 1e6,
           fi.StopFrequency_Hz / 1e6, fi.FrameWidth, fi.FrameHeight, fi.PacketCount);
    uint8_t *trace = malloc(fi.PacketValidPoints);
    uint16_t *bitmap = malloc((size_t)fi.FrameHeight * fi.FrameWidth * 2);
    RTA_PlotInfo_TypeDef plot;
    RTA_TriggerInfo_TypeDef trig;
    MeasAuxInfo_TypeDef aux;
    CHECK("RTA_BusTriggerStart", RTA_BusTriggerStart(dev));
    int ok = 0, n = 200;
    double t0 = now_s();
    float peak = -999;
    for (int i = 0; i < n; i++) {
        int rs = RTA_GetRealTimeSpectrum(dev, trace, bitmap, &plot, &trig, &aux);
        if (i < 3 || (rs && i == n - 1)) printf("  frame %d status %d\n", i, rs);
        if (rs == 0) {
            ok++;
            for (uint32_t k = 0; k < fi.FrameWidth; k++) {
                float dbm = trace[k] * plot.ScaleTodBm + plot.OffsetTodBm;
                if (dbm > peak) peak = dbm;
            }
        }
    }
    printf("  %d/%d frames ok in %.2f s, peak %.1f dBm\n", ok, n, now_s() - t0, peak);
    if (ok != n) failures++;
    CHECK("RTA_BusTriggerStop", RTA_BusTriggerStop(dev));
    free(trace);
    free(bitmap);
}

static void test_iqs_dsp(void **dev) {
    printf("[IQS + DSP FFT]\n");
    IQS_Profile_TypeDef in, out;
    IQS_StreamInfo_TypeDef si;
    IQS_ProfileDeInit(dev, &in);
    in.CenterFreq_Hz = 100e6;
    in.RefLevel_dBm = -20;
    in.DecimateFactor = getenv("HTRA_DECIM") ? atoi(getenv("HTRA_DECIM")) : 2;
    in.DataFormat = Complex16bit;
    in.TriggerSource = Bus;
    in.TriggerMode = Adaptive;
    int st = IQS_Configuration(dev, &in, &out, &si);
    CHECK("IQS_Configuration", st);
    if (st < 0) return;
    printf("  IQ sample rate %.3f MS/s, %u samples/packet\n", si.IQSampleRate / 1e6,
           si.PacketSamples);

    void *dsp = NULL;
    CHECK("DSP_Open", DSP_Open(&dsp));
    DSP_FFT_TypeDef fin, fout;
    DSP_FFT_DeInit(&fin);
    fin.FFTSize = 4096;
    fin.SamplePts = si.PacketSamples;
    uint32_t pts = 0;
    double rbw_ratio = 0;
    CHECK("DSP_FFT_Configuration", DSP_FFT_Configuration(&dsp, &fin, &fout, &pts, &rbw_ratio));
    double *freq = calloc(pts ? pts : 1, sizeof *freq);
    float *pwr = calloc(pts ? pts : 1, sizeof *pwr);

    CHECK("IQS_BusTriggerStart", IQS_BusTriggerStart(dev));
    IQStream_TypeDef iq;
    int ok = 0, n = 500, nonzero = 0, fft_ok = 0;
    double t0 = now_s();
    for (int i = 0; i < n; i++) {
        if (IQS_GetIQStream_PM1(dev, &iq) != 0) continue;
        ok++;
        const int16_t *s = iq.AlternIQStream;
        for (uint32_t k = 0; k < 64; k++)
            if (s[k]) {
                nonzero++;
                break;
            }
        if (i % 50 == 0 && pts && DSP_FFT_IQSToSpectrum(&dsp, &iq, freq, pwr) == 0) fft_ok++;
    }
    double dt = now_s() - t0;
    printf("  %d/%d packets ok (%d with signal), %.1f MS/s sustained, %d FFTs ok (%u pts)\n", ok,
           n, nonzero, ok * (double)si.PacketSamples / dt / 1e6, fft_ok, pts);
    if (ok != n || !nonzero) failures++;
    if (pts) printf("  FFT axis %.3f..%.3f MHz\n", freq[0] / 1e6, freq[pts - 1] / 1e6);
    CHECK("IQS_BusTriggerStop", IQS_BusTriggerStop(dev));
    DSP_Close(&dsp);
    free(freq);
    free(pwr);
}

static void test_det(void **dev) {
    printf("[DET]\n");
    DET_Profile_TypeDef in, out;
    DET_StreamInfo_TypeDef si;
    DET_ProfileDeInit(dev, &in);
    in.CenterFreq_Hz = 100e6;
    in.TriggerSource = Bus;
    in.TriggerMode = FixedPoints;
    int st = DET_Configuration(dev, &in, &out, &si);
    CHECK("DET_Configuration", st);
    if (st < 0) return;
    float *buf = calloc(si.PacketSamples ? si.PacketSamples : 1, sizeof *buf);
    float scale;
    DET_TriggerInfo_TypeDef trig;
    MeasAuxInfo_TypeDef aux;
    CHECK("DET_BusTriggerStart", DET_BusTriggerStart(dev));
    int ok = 0;
    for (uint32_t i = 0; i < si.PacketCount; i++)
        if (DET_GetPowerStream(dev, buf, &scale, &trig, &aux) == 0) ok++;
    printf("  %d/%u packets ok\n", ok, si.PacketCount);
    CHECK("DET_BusTriggerStop", DET_BusTriggerStop(dev));
    free(buf);
}

static void test_mscan(void **dev) {
    printf("[MSCAN]\n");
    MSCAN_Profile_TypeDef in[4], out[4];
    int32_t elements = 4;
    int64_t reps = 20;
    PreamplifierState_TypeDef pre = AutoOn;
    MSCAN_Info_Typedef info[4];
    CHECK("MSCAN_ProfileDeinit", MSCAN_ProfileDeinit(dev, in, &elements));
    for (int i = 0; i < 4; i++) in[i].CenterFreq_Hz = 470e6 + i * 25e6;
    int st = MSCAN_Configuration(dev, in, out, info, &elements, &reps, &pre);
    CHECK("MSCAN_Configuration", st);
    if (st < 0) return;
    CHECK("MSCAN_Start", MSCAN_Start(dev));
    MSCAN_Data_Typedef d;
    int ok = 0, tries = 0;
    double t0 = now_s();
    int other = 0;
    double level = -999;
    while (ok < elements * reps && now_s() - t0 < 10) {
        int r = MSCAN_GetData(dev, &d);
        tries++;
        if (r == 0) {
            ok++;
            if (d.SpectrumPoints && d.SpectrumStream)
                level = d.SpectrumStream[d.SpectrumPoints / 2] * d.ScaleTodBm + d.OffsetTodBm;
        } else if (r == APIRETVAL_WARNING_DataNotReady) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        } else if (other++ < 3) {
            printf("  MSCAN_GetData -> %d\n", r);
        }
    }
    printf("  last element %d, repeat %lld, centre level %.1f dBm\n", d.ElementIndex,
           (long long)d.RepeatIndex, level);
    printf("  %d/%lld results in %.2f s\n", ok, (long long)(elements * reps), now_s() - t0);
    if (!ok) failures++;
    CHECK("MSCAN_Stop", MSCAN_Stop(dev));
}

static void stress_swp(void **dev, int sweeps) {
    printf("[SWP stress: %d full sweeps 50 MHz-6 GHz]\n", sweeps);
    SWP_Profile_TypeDef in, out;
    SWP_TraceInfo_TypeDef ti;
    SWP_ProfileDeInit(dev, &in);
    in.StartFreq_Hz = 50e6;
    in.StopFreq_Hz = 6e9;
    in.RBWMode = RBW_Manual;
    in.RBW_Hz = 300e3;
    int st = SWP_Configuration(dev, &in, &out, &ti);
    CHECK("SWP_Configuration", st);
    if (st < 0) return;
    double *f = calloc(ti.FullsweepTracePoints, sizeof *f);
    float *p = calloc(ti.FullsweepTracePoints, sizeof *p);
    MeasAuxInfo_TypeDef aux;
    int bad = 0, nan = 0;
    double t0 = now_s();
    for (int s = 0; s < sweeps; s++) {
        if (SWP_GetFullSweep(dev, f, p, &aux) < 0) bad++;
        for (int i = 0; i < ti.FullsweepTracePoints; i += 97)
            if (p[i] != p[i] || p[i] < -200 || p[i] > 50) nan++;
    }
    double dt = now_s() - t0;
    printf("  %d sweeps x %d pts in %.1f s (%.1f sweeps/s), %d errors, %d bad samples\n", sweeps,
           ti.FullsweepTracePoints, dt, sweeps / dt, bad, nan);
    if (bad || nan) failures++;
    free(f);
    free(p);
}

static int want(int argc, char **argv, const char *mode) {
    if (argc < 2) return 1;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], mode)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int sweeps = getenv("HTRA_SWEEPS") ? atoi(getenv("HTRA_SWEEPS")) : 200;
    void *dev = NULL;
    BootProfile_TypeDef boot;
    BootInfo_TypeDef info;
    memset(&boot, 0, sizeof boot);
    boot.PhysicalInterface = USB;
    boot.DevicePowerSupply = getenv("HTRA_USB_POWER_ONLY") ? USBPortOnly : USBPortAndPowerPort;
    int st = Device_Open(&dev, 0, &boot, &info);
    printf("Device_Open: %d, API 0x%x, UID %016llx\n", st, Get_APIVersion(),
           (unsigned long long)info.DeviceInfo.DeviceUID);
    if (st < 0) return 1;
    if (want(argc, argv, "state")) test_state(&dev);
    if (want(argc, argv, "swp")) stress_swp(&dev, sweeps);
    if (want(argc, argv, "rta")) test_rta(&dev);
    if (want(argc, argv, "iqs")) test_iqs_dsp(&dev);
    if (want(argc, argv, "det")) test_det(&dev);
    if (want(argc, argv, "mscan")) test_mscan(&dev);
    printf("Device_Close: %d\n", Device_Close(&dev));
    printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures != 0;
}
