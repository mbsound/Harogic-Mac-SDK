/*
 * Hardware check: open the first USB analyzer, print its identity, then run
 * a few full SWP sweeps over the FM broadcast band and report the peaks.
 *
 *   HTRAAPI_DATA_DIR=<dir containing CalFile/> ./build/swp_test [start_MHz stop_MHz]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    double *freq = calloc(ti.FullsweepTracePoints, sizeof *freq);
    float *pwr = calloc(ti.FullsweepTracePoints, sizeof *pwr);
    MeasAuxInfo_TypeDef aux;
    for (int sweep = 0; sweep < 5; sweep++) {
        int hop = 0, frame = 0, errors = 0;
        t0 = now_s();
        for (int i = 0; i < ti.TotalHops; i++) {
            st = SWP_GetPartialSweep(&dev, freq + i * ti.PartialsweepTracePoints,
                                     pwr + i * ti.PartialsweepTracePoints, &hop, &frame, &aux);
            if (st != APIRETVAL_NoError) errors++;
        }
        double dt = now_s() - t0;
        if (sweep == 0) {
            int P = ti.PartialsweepTracePoints, N = ti.FullsweepTracePoints;
            printf("  freq axis: [0]=%.6f [1]=%.6f [P-1]=%.6f [P]=%.6f [N-1]=%.6f MHz (P=%d)\n",
                   freq[0] / 1e6, freq[1] / 1e6, freq[P - 1] / 1e6, freq[P] / 1e6, freq[N - 1] / 1e6, P);
            printf("  raw UserStartIndex=%u UserStopIndex=%u StartFreq=%.6f MHz AnalysisBW=%.3f MHz\n", ti.UserStartIndex, ti.UserStopIndex, ti.StartFreq_Hz / 1e6, ti.AnalysisBW_Hz / 1e6);
            printf("  user span: index %u..%u -> %.6f..%.6f MHz (bin %.1f Hz)\n", ti.UserStartIndex,
                   (N - P) + ti.UserStopIndex, freq[ti.UserStartIndex] / 1e6,
                   freq[(N - P) + ti.UserStopIndex] / 1e6, ti.TraceBinBW_Hz);
        }
        /* Report the three strongest local maxima. */
        int top[3] = {-1, -1, -1};
        int lo = (int)ti.UserStartIndex;
        int hi = (ti.FullsweepTracePoints - ti.PartialsweepTracePoints) + (int)ti.UserStopIndex;
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
    st = Device_Close(&dev);
    printf("Device_Close: %d\n", st);
    return 0;
}
