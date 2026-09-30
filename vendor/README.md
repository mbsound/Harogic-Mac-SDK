# Harogic Linux SDK

`vendor/Linux_API/` is Harogic's own aarch64 Linux SDK, unmodified, which this
port loads at run time. It is the version the port is built and tested against:

| | |
|---|---|
| SDK | 0.55.89 (API version `0x3759`), Harogic's `Linux_API-SAN-828` package |
| Libraries used | `libhtraapi.so.0.55.89`, `libliquid.so`, `libDigitalSigDemod.so` (aarch64) |
| Tested analyzer | SAN-60 (model `0x42`), firmware `0x3768` (0.55.104), USB 3 |

The Harogic SDK is © Harogic and is included here, unchanged, so the port builds
out of the box. It is not covered by this repository's MIT licence.

## Firmware compatibility

Each SDK release only opens analyzers whose firmware matches its baseline;
otherwise `Device_Open` returns -49 (`FirmwareVersionMismatch`), exactly as on
Linux. Use the SDK that matches your analyzer's firmware:

| SDK | API | Opens firmware 0.55.104 (`0x3768`)? | Notes |
|---|---|---|---|
| 0.55.88 | `0x3758` | ❌ -49 | |
| 0.55.89 (`SAN-828` package, included) | `0x3759` | ✅ | |
| 0.55.89 (generic `Install_Linux_API` package) | `0x3759` | not tested | A different, older build with the same version number: lacks `Device_GetSupportedFunctions`, `Device_GetDecimateFactorList`, `Device_GetBusBandwidth`, `Device_GetOnboardMemorySize` and the demod plugin. |
| 0.55.100 (`SA_Linux_API`) | `0x3764` | ❌ -49 | Builds and loads (all imports resolved); expects newer firmware (the library references 0.55.109). |

## Using another SDK version

Unzip it anywhere and pass its location:

```bash
make SDK=/path/to/Linux_API
```

The build takes the newest `libhtraapi.so.*`, `libliquid.so` and (if present)
`libDigitalSigDemod.so` from `htraapi/lib/aarch64/`.
