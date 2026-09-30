/*
 * net_test: discover and use an Ethernet-attached analyzer.
 *
 *   net_test [ip] [local-ip local-mask]
 *
 * 1. Device_GetNetworkDeviceList, searching from local-ip/local-mask (default:
 *    the first address of the ip's /24, e.g. 192.168.1.0/24).
 * 2. Device_Open over ETH to ip (default: the first analyzer discovered).
 * 3. Identify, power readout, 20 sweeps of 88-108 MHz, 5 s of IQS at 1/64 rate.
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

static bool parse_ip(const char *s, uint8_t out[4]) {
    unsigned a, b, c, d;
    if (!s || sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    out[0] = a, out[1] = b, out[2] = c, out[3] = d;
    return true;
}

static int failures;

static void sweeps(void **dev) {
    SWP_Profile_TypeDef in, out;
    SWP_TraceInfo_TypeDef ti;
    SWP_ProfileDeInit(dev, &in);
    in.StartFreq_Hz = 88e6;
    in.StopFreq_Hz = 108e6;
    in.RBW_Hz = 30e3;
    int st = SWP_Configuration(dev, &in, &out, &ti);
    printf("SWP_Configuration: %d (%d points, %d hops)\n", st, ti.FullsweepTracePoints, ti.TotalHops);
    if (st < 0) {
        failures++;
        return;
    }
    double *freq = malloc(sizeof(double) * ti.FullsweepTracePoints);
    float *pwr = malloc(sizeof(float) * ti.FullsweepTracePoints);
    MeasAuxInfo_TypeDef aux;
    int ok = 0, n = 20;
    double t0 = now_s();
    for (int i = 0; i < n; i++) {
        int s = SWP_GetFullSweep(dev, freq, pwr, &aux);
        if (s == 0 || s == -12) ok++;
        else if (i < 3) printf("  sweep %d status %d\n", i, s);
    }
    float peak = -999;
    int pk = 0;
    for (int k = 0; k < ti.FullsweepTracePoints; k++)
        if (pwr[k] > peak) peak = pwr[k], pk = k;
    printf("  %d/%d sweeps ok in %.2f s; peak %.1f dBm at %.3f MHz\n", ok, n, now_s() - t0, peak,
           freq[pk] / 1e6);
    if (ok != n) failures++;
    free(freq);
    free(pwr);
}

static void iqs(void **dev) {
    IQS_Profile_TypeDef in, out;
    IQS_StreamInfo_TypeDef si;
    IQS_ProfileDeInit(dev, &in);
    in.CenterFreq_Hz = 100e6;
    in.RefLevel_dBm = -20;
    in.DecimateFactor = 64;
    in.DataFormat = Complex16bit;
    in.TriggerSource = Bus;
    in.TriggerMode = Adaptive;
    int st = IQS_Configuration(dev, &in, &out, &si);
    printf("IQS_Configuration: %d (%.3f MS/s, %u samples/packet)\n", st, si.IQSampleRate / 1e6,
           si.PacketSamples);
    if (st < 0) {
        failures++;
        return;
    }
    void *data;
    float scale;
    IQS_TriggerInfo_TypeDef trig;
    MeasAuxInfo_TypeDef aux;
    long ok = 0, err = 0;
    int last = 0;
    IQS_BusTriggerStart(dev);
    double t0 = now_s();
    while (now_s() - t0 < 5) {
        int s = IQS_GetIQStream(dev, &data, &scale, &trig, &aux);
        if (s == 0 || s == -12) ok++;
        else err++, last = s;
        if (err > 50) break;
    }
    double dt = now_s() - t0;
    IQS_BusTriggerStop(dev);
    double rate = ok * (double)si.PacketSamples / dt;
    printf("  %ld packets, %.3f MS/s = %.1f%% of the IQ rate, %ld errors%s\n", ok, rate / 1e6,
           100 * rate / si.IQSampleRate, err, err ? "" : "");
    if (err) printf("  last error: %d\n", last);
    if (err || rate < 0.99 * si.IQSampleRate) failures++;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    uint8_t target[4] = {0}, local_ip[4] = {0}, local_mask[4] = {255, 255, 255, 0};
    bool have_target = parse_ip(argc > 1 ? argv[1] : NULL, target);
    if (argc > 3) {
        parse_ip(argv[2], local_ip);
        parse_ip(argv[3], local_mask);
    } else if (have_target) {
        memcpy(local_ip, target, 3); /* search the target's /24 */
    }

    /* 1. Discovery */
    uint8_t count = 0;
    NetworkDeviceInfo_TypeDef list[64];
    memset(list, 0, sizeof list);
    double t0 = now_s();
    int st = Device_GetNetworkDeviceList(&count, list, local_ip, local_mask);
    printf("Device_GetNetworkDeviceList(%u.%u.%u.%u/%u.%u.%u.%u): %d, %u device(s) in %.2f s\n",
           local_ip[0], local_ip[1], local_ip[2], local_ip[3], local_mask[0], local_mask[1],
           local_mask[2], local_mask[3], st, count, now_s() - t0);
    for (unsigned i = 0; i < count; i++)
        printf("  %u.%u.%u.%u  model 0x%x  UID %016llx  firmware 0x%x/0x%x\n", list[i].IPAddress[0],
               list[i].IPAddress[1], list[i].IPAddress[2], list[i].IPAddress[3], list[i].Model,
               (unsigned long long)list[i].DeviceUID, list[i].MFWVersion, list[i].FFWVersion);
    if (!have_target) {
        if (!count) {
            printf("No analyzer found; pass its IP address.\n");
            return 1;
        }
        memcpy(target, list[0].IPAddress, 4);
    }

    /* 2. Open over Ethernet */
    void *dev = NULL;
    BootProfile_TypeDef boot;
    BootInfo_TypeDef info;
    memset(&boot, 0, sizeof boot);
    boot.PhysicalInterface = ETH;
    boot.DevicePowerSupply = Others;
    boot.ETH_IPVersion = IPv4;
    memcpy(boot.ETH_IPAddress, target, 4);
    boot.ETH_RemotePort = getenv("HTRA_PORT") ? atoi(getenv("HTRA_PORT")) : 5000;
    boot.ETH_ReadTimeOut = 5000;
    t0 = now_s();
    st = Device_Open(&dev, 0, &boot, &info);
    printf("Device_Open(ETH %u.%u.%u.%u:%u): %d in %.2f s (ETH code %d)\n", target[0], target[1],
           target[2], target[3], boot.ETH_RemotePort, st, now_s() - t0, boot.ETH_ErrorCode);
    if (st < 0) return 1;
    printf("  model 0x%x, UID %016llx, firmware 0x%x/0x%x, API 0x%x\n", info.DeviceInfo.Model,
           (unsigned long long)info.DeviceInfo.DeviceUID, info.DeviceInfo.MFWVersion,
           info.DeviceInfo.FFWVersion, Get_APIVersion());
    PowerSupplyState_TypeDef ps;
    if (Device_QueryPowerSupplyState(&dev, &ps) == 0)
        printf("  power port %.2f V %.2f A, USB %.2f V %.2f A\n", ps.rf_vlotage, ps.rf_current,
               ps.usb_vlotage, ps.usb_current);

    /* 3. Measurements */
    sweeps(&dev);
    iqs(&dev);
    printf("Device_Close: %d\n%s\n", Device_Close(&dev), failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
