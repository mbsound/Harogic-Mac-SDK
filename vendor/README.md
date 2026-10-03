# Harogic Linux SDK

`vendor/Linux_API/` and `vendor/Linux_API-0.55.100/` are Harogic's own aarch64 Linux
SDKs, unmodified, which this port loads at run time. They are the versions the port is
built and tested against (only the parts the port uses are included: the aarch64
libraries below, the headers and `htrausb.conf`):

| | `vendor/Linux_API/` (the default) | `vendor/Linux_API-0.55.100/` |
|---|---|---|
| SDK | 0.55.89 (API version `0x3759`), Harogic's `Linux_API-SAN-828` package | 0.55.100 (API version `0x3764`), Harogic's `SA_Linux_API` package |
| Libraries used | `libhtraapi.so.0.55.89`, `libliquid.so`, `libDigitalSigDemod.so` | `libhtraapi.so.0.55.100`, `libliquid.so`, `libDigitalSigDemod.so` |
| For firmware | 0.55.104 (`0x3768`) | 0.55.109 (`0x376d`) |
| Tested analyzer | SAN-60 (model `0x42`) over USB 3; an Ethernet analyzer (model `0x43`) | SAN-60 (model `0x42`) over USB, including its GNSS receiver |
| Build | `make` | `make SDK=vendor/Linux_API-0.55.100` |

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
| 0.55.100 (`SA_Linux_API`, in `vendor/Linux_API-0.55.100/`) | `0x3764` | ❌ -49 | For firmware 0.55.109 (`0x376d`): opens it (0.55.89 then returns -49) and is the build through which the SAN-60's GNSS receiver reports (`Device_GetGNSSInfo`: satellites, fix, position, UTC). Build with `make SDK=vendor/Linux_API-0.55.100`. |

## Using another SDK version

Unzip it anywhere and pass its location:

```bash
make SDK=/path/to/Linux_API
```

The build takes the newest `libhtraapi.so.*`, `libliquid.so` and (if present)
`libDigitalSigDemod.so` from `htraapi/lib/aarch64/`.
