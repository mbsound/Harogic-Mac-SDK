# Harogic Linux SDK goes here

This project builds a macOS library from Harogic's own **aarch64 Linux SDK**,
which is not included in this repository. Download the Linux SDK from Harogic
(e.g. `Linux_API-0-55-89.zip`) and unzip it here so that this path exists:

```
vendor/Linux_API/htraapi/lib/aarch64/libhtraapi.so.*
```

Or leave it anywhere and pass its location: `make SDK=/path/to/Linux_API`.

The analyzer's firmware must match the SDK version (`Device_Open` returns -49,
`FirmwareVersionMismatch`, otherwise). Use the SDK version that matches your
device, as you would on Linux.
