/*
 * Hardware check: open the first USB analyzer, print its identity, then run
 * a few full SWP sweeps over the FM broadcast band and report the peaks.
 *
 *   HTRAAPI_DATA_DIR=<dir containing CalFile/> ./build/swp_test [start_MHz stop_MHz]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include <stdbool.h>
#include <stdint.h>

#include "htra_api.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    double start = argc > 2 ? atof(argv[1]) * 1e6 : 88e6;
    double stop = argc > 2 ? atof(argv[2]) * 1e6 : 108e6;

    printf("API version: 0x%08x\n", Get_APIVersion());

    void *dev = NULL;
    BootProfile_TypeDef boot;
    BootInfo_TypeDef info;
    memset(&boot, 0, sizeof boot);
    memset(&info, 0, sizeof info);
    boot.PhysicalInterface = USB;
    boot.DevicePowerSupply = USBPortAndPowerPort;

    double t0 = now_s();
    int st = Device_Open(&dev, 0, &boot, &info);
    printf("Device_Open: %d (%.2f s)\n", st, now_s() - t0);
    if (st < 0) return 1;
    printf("  UID %016llx  model 0x%x  HW %u  MCU fw 0x%x  FPGA fw 0x%x\n",
           (unsigned long long)info.DeviceInfo.DeviceUID, info.DeviceInfo.Model,
           info.DeviceInfo.HardwareVersion, info.DeviceInfo.MFWVersion,
           info.DeviceInfo.FFWVersion);
    printf("  bus speed %u, errors %d, warnings %d", info.BusSpeed, info.Errors, info.Warnings);
    for (int i = 0; i < info.Warnings && i < 7; i++) printf(" [W%d]", info.WarningCodes[i]);
    for (int i = 0; i < info.Errors && i < 7; i++) printf(" [E%d]", info.ErrorCodes[i]);
    printf("\n");

    SWP_Profile_TypeDef in, out;
    SWP_TraceInfo_TypeDef ti;
    SWP_ProfileDeInit(&dev, &in);
    in.StartFreq_Hz = start;
    in.StopFreq_Hz = stop;
    in.RBWMode = RBW_Manual;
    in.RBW_Hz = 30e3;
    st = SWP_Configuration(&dev, &in, &out, &ti);
    printf("SWP_Configuration: %d  %.3f-%.3f MHz RBW %.0f Hz, %d points, %d hops\n", st,
           out.StartFreq_Hz / 1e6, out.StopFreq_Hz / 1e6, out.RBW_Hz,
           ti.FullsweepTracePoints, ti.TotalHops);
    if (st < 0) {
        Device_Close(&dev);
        return 1;
    }

    /* A single hop can be longer than the whole trace (88-108 MHz: P=3040,
     * N=1312), so size the buffers by hops, not by FullsweepTracePoints. */
    int P = ti.PartialsweepTracePoints, T = ti.TotalHops;
    /* UserStopIndex isn't reliable for this: at 88-108 MHz with the default
     * profile it reads 0 over 3 hops, which would put the end at 146 MHz.
     * Find the user span on the frequency axis instead. */
    int first = 0, last = T * P - 1;
    double *freq = calloc((size_t)T * P, sizeof *freq);
    float *pwr = calloc((size_t)T * P, sizeof *pwr);
    double *hf = calloc(P, sizeof *hf);
    float *hp = calloc(P, sizeof *hp);
    MeasAuxInfo_TypeDef aux;
    for (int sweep = 0; sweep < 5; sweep++) {
        int hop = 0, frame = 0, errors = 0, got = 0, extra = 0;
        char *seen = calloc(T, 1);
        t0 = now_s();
        /* With spur rejection on, the SDK can return extra frames (hop >=
         * TotalHops, at unrelated frequencies), so read until each hop has
         * arrived once rather than exactly TotalHops times. */
        for (int i = 0; i < 4 * T + 8 && got < T && errors < T; i++) {
            st = SWP_GetPartialSweep(&dev, hf, hp, &hop, &frame, &aux);
            if (st != APIRETVAL_NoError) { errors++; continue; }
            if (hop < 0 || hop >= T || seen[hop]) { extra++; continue; }
            if (fabs(hf[0] - (ti.StartFreq_Hz + (double)hop * P * ti.TraceBinBW_Hz)) > 2 * ti.TraceBinBW_Hz) { extra++; continue; }
            seen[hop] = 1; got++;
            memcpy(freq + (size_t)hop * P, hf, P * sizeof *hf);
            memcpy(pwr + (size_t)hop * P, hp, P * sizeof *hp);
        }
        free(seen);
        double dt = now_s() - t0;
        if (sweep == 0) {
            double half = ti.TraceBinBW_Hz / 2;
            while (first < last && freq[first] < out.StartFreq_Hz - half) first++;
            while (last > first && freq[last] > out.StopFreq_Hz + half) last--;
        }
        if (sweep == 0) {
            printf("  hops %d/%d, %d extra frame(s) skipped\n", got, T, extra);
            printf("  freq axis: [0]=%.6f [1]=%.6f [P-1]=%.6f [last]=%.6f MHz (P=%d)\n",
                   freq[0] / 1e6, freq[1] / 1e6, freq[P - 1] / 1e6, freq[last] / 1e6, P);
            printf("  raw UserStartIndex=%u UserStopIndex=%u StartFreq=%.6f MHz AnalysisBW=%.3f MHz\n", ti.UserStartIndex, ti.UserStopIndex, ti.StartFreq_Hz / 1e6, ti.AnalysisBW_Hz / 1e6);
            printf("  user span: index %d..%d -> %.6f..%.6f MHz (bin %.1f Hz)\n", first,
                   last, freq[first] / 1e6, freq[last] / 1e6, ti.TraceBinBW_Hz);
        }
        /* Report the three strongest local maxima. */
        int top[3] = {-1, -1, -1};
        int lo = first;
        int hi = last;
        for (int i = lo + 1; i < hi; i++) {
            if (pwr[i] < pwr[i - 1] || pwr[i] < pwr[i + 1]) continue;
            for (int k = 0; k < 3; k++)
                if (top[k] < 0 || pwr[i] > pwr[top[k]]) {
                    memmove(&top[k + 1], &top[k], (2 - k) * sizeof(int));
                    top[k] = i;
                    break;
                }
        }
        double floor_sum = 0;
        for (int i = lo; i <= hi; i++) floor_sum += pwr[i];
        printf("sweep %d: %.0f ms, status %d (%d hop errors), mean %.1f dBm, peaks:", sweep,
               dt * 1e3, st, errors, floor_sum / (hi - lo + 1));
        for (int k = 0; k < 3; k++)
            if (top[k] >= 0) printf("  %.3f MHz %.1f dBm", freq[top[k]] / 1e6, pwr[top[k]]);
        printf("\n");
    }
    free(freq);
    free(pwr);
    free(hf);
    free(hp);
    st = Device_Close(&dev);
    printf("Device_Close: %d\n", st);
    return 0;
}
