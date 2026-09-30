# Harogic SDK for macOS (Apple Silicon)

A native `libhtraapi.dylib` for arm64 macOS that runs Harogic's **unmodified**
aarch64 Linux SDK (`libhtraapi.so`, `libliquid.so`, `libDigitalSigDemod.so`)
inside a normal Mac process. It exports the same C API as the Linux/Windows
SDK, so existing C, C++ and Python (`ctypes`) code works unchanged.

Tested with SDK 0.55.89 and a SAN-60 (model 0x42, firmware 0x3768) over USB 3.

## How it works

The vendor library is closed source, but its aarch64 build uses the same
instruction set as Apple Silicon. So instead of emulating or reverse-engineering
the USB protocol, this project loads the Linux binaries directly:

| Piece | What it does |
|---|---|
| `src/elf_loader.c` | Maps the ELF objects, applies relocations, registers `.eh_frame` with the system unwinder (C++ exceptions work), runs constructors. |
| `tools/x18_patch.py` | **x18:** macOS zeroes register x18 on syscalls/preemption; GCC uses it as a scratch register. Every x18 instruction is redirected to a stub that keeps the value in a free SIMD register. Each rewrite is verified by disassembly. |
| `src/shim_libc.c`, `shim_asm.S` | glibc → libSystem: variadic calls (Linux passes varargs in registers, Apple on the stack), `errno` values, `open`/`fcntl` flags, `dirent`, `_FORTIFY_SOURCE` entry points, `/proc/self/exe`, `/etc/htrausb.conf`. |
| `src/shim_pthread.c` | glibc and macOS pthread objects differ in size and initializers; Linux-side objects hold pointers to native ones. |
| `src/shim_cxx.c`, `shim_fstream.cpp` | Runs against Homebrew GCC's libstdc++. Bridges the parts whose layout differs between Linux and Darwin: `condition_variable`, futexes, `ctype<char>` tables, file streams (`mutex`/`mbstate_t` sizes), and `tellg` (`streampos` is 16 vs 136 bytes). |
| `src/shim_net.c` | epoll on kqueue, socket constants and `sockaddr` layout, `getifaddrs`, UDP broadcast discovery (Ethernet-attached analyzers). |
| `src/shim_alloc.cpp` | Pads every vendor heap allocation by 16 bytes. glibc's chunk slack hides small overruns in the vendor code (e.g. a 1-byte overrun in `Device_GetByteStream_IFACalData` on every open); macOS's allocator doesn't, and aborts later. |
| `src/shim_usb.c` | libusb argument widening, plus streaming read-ahead (see below). |
| `tools/gen_build.py` | Embeds the vendor ELFs in the dylib and generates the exported API trampolines. |

## Build

1. Install the tools: `brew install gcc libusb binutils` (plus Xcode command
   line tools).
2. The Harogic Linux SDK the port is tested with (0.55.89) is included in
   `vendor/Linux_API/`. For another version, pass `SDK=` (below). The SDK must
   match your analyzer's firmware; see
   [vendor/README.md](vendor/README.md#firmware-compatibility).
3. Build:

```bash
make                            # build/libhtraapi.dylib
make dist                       # dist/htraapi-macos-arm64/: relocatable, self-contained
make examples                   # build/swp_test, build/mode_test
make SDK=/path/to/Linux_API     # SDK somewhere else / another version
```

Nothing is installed system-wide: no `sudo`, no udev rules, no `/etc` files.

`dist/htraapi-macos-arm64/lib/` bundles libusb and GCC's libstdc++ with
`@loader_path` references, so the folder can be copied anywhere (e.g. into an
app). Headers are in `dist/htraapi-macos-arm64/include/`.

## Use

**Prebuilt:** download `Harogic-Mac-SDK-<version>-arm64.zip` from
[Releases](https://github.com/mbsound/Harogic-Mac-SDK/releases): no Homebrew or
build needed. Its README covers first-run setup.

```python
import ctypes
dll = ctypes.CDLL("dist/htraapi-macos-arm64/lib/libhtraapi.dylib")
```

In C, include `<stdbool.h>` and `<stdint.h>` before `htra_api.h` (Harogic's
header uses `bool`).

```bash
clang -Idist/htraapi-macos-arm64/include app.c -Ldist/htraapi-macos-arm64/lib -lhtraapi \
      -Wl,-rpath,@executable_path/../lib
```

**Calibration files.** The SDK looks for `CalFile/` next to the executable. On
macOS the directory is chosen in this order:

1. `$HTRAAPI_DATA_DIR/CalFile`
2. `CalFile/` next to the real executable (native apps)
3. `./CalFile` in the working directory
4. `~/Library/Application Support/htraapi/CalFile`, created automatically.

If files are missing, the SDK reads calibration from the analyzer's flash and
caches it there. Those copies were verified byte-identical to the vendor's
files.

## Environment variables

| Variable | Effect |
|---|---|
| `HTRAAPI_DATA_DIR` | Directory holding `CalFile/` and an optional `htrausb.conf`. |
| `HTRAAPI_TRACE=1` | Loader/shim log, plus a crash report with vendor symbol names. |
| `HTRAAPI_TRACE_IMPORTS=1` | Log every call the vendor code makes into libc/libusb/liquid. |
| `HTRAAPI_TRACE_NET=1` | Log socket sends/receives and epoll wakeups with timestamps (Ethernet debugging). |
| `HTRAAPI_USB_READAHEAD=n` | Streaming read-ahead queue depth (default 16; 0 disables). |

## Status (SAN-60, SDK 0.55.89)

| Mode | Result |
|---|---|
| Device open / info / state / close, calibration load from files or flash | ✅ |
| SWP: 1000 full 50 MHz–6 GHz sweeps at ~87 sweeps/s, zero errors | ✅ |
| RTA, IQS (+ DSP FFT/DDC), DET, MSCAN | ✅ |
| IQS at 62.5 MS/s (250 MB/s), 5-minute soak | ✅ 100.0% of the rate, 0 errors, 0 dropped packets (analyzer on its power supply) |
| RTA at full bandwidth (101.6 MHz span), 60 s | ✅ 100.0% real-time coverage, 0 errors |
| IQS at decimation 1 (125 MS/s, 500 MB/s) | ❌ more than USB 3 (5 Gbit/s) sustains in practice; stalls |
| Supply voltage/current (`Device_QueryPowerSupplyState`) | ✅ live, also while streaming |
| `libDigitalSigDemod` plugin | ✅ loads; `Demod_Open` needs Harogic's demod licence file (-60 otherwise, same as Linux) |
| PCIe devices | ❌ Linux kernel driver only |
| Ethernet (tested: model 0x43 on 1 GbE) | ✅ open, sweeps, IQS at 100% of the rate. See [Ethernet analyzers](#ethernet-analyzers). |

**Power.** For high-rate streaming, run the analyzer from its power supply.
Bus-powered, it dropped off USB at 62.5 MS/s, and at 31 MS/s after sustained
activity (`kIOReturnNotResponding`, then a hotplug detach). On its power supply
the same tests run clean: the SAN-60 drew about 10 W from the power port, and
the USB port current rose to 0.85 A during RTA, close to USB 3's 0.9 A port
limit. `Device_QueryPowerSupplyState` reports voltage and current on both
ports (the figures SAStudio shows); `build/stream_test` prints them.

## Ethernet analyzers

Open with `PhysicalInterface = ETH`, `DevicePowerSupply = Others`,
`ETH_IPVersion = IPv4`, the analyzer's address in `ETH_IPAddress[0..3]`, and
`ETH_RemotePort = 5000` (see `examples/net_test.c`).

- **Discovery:** `Device_GetNetworkDeviceList` and `Device_SetNetworkDeviceIP` are
  stubs in Harogic's Linux SDK (they return 10068/10069 without touching the
  network), so they are on macOS too. Connect by IP, or find analyzers by probing
  TCP ports 5000 and 9000 on the local subnet.
- **Stalled connections:** roughly 1 in 4 opens takes ~21 s instead of ~3 s. The
  analyzer stops answering part-way through the handshake and `Device_Open` only
  returns after the SDK's read timeouts (6 × `ETH_ReadTimeOut`); that session is
  dead, and every later call fails with 10060. A socket-level trace shows the Mac
  sending every byte and closing cleanly, so this appears to be the analyzer or the
  SDK's protocol (not yet compared with Linux). Workaround: if `Device_Open` takes
  much longer than usual (e.g. >12 s), `Device_Close` and open again. A shorter
  `ETH_ReadTimeOut` does not help: below ~2 s normal opens fail (-3), because the
  analyzer takes ~2 s to answer the first request.
- `SO_RCVBUF`: the SDK asks for 32 MB; like Linux, the shim caps it at the system
  maximum instead of failing.

## Examples

Build them with `make examples`:

- `examples/swp_test.c`: open, identify, sweep, report peaks.
- `examples/mode_test.c [state swp rta iqs det mscan]`: exercises every mode.
  Set `HTRA_DECIM` for IQS/RTA decimation and `HTRA_SWEEPS` for the SWP stress
  count.
- `examples/net_test.c [ip]`: open an Ethernet analyzer, power readout, 20
  sweeps and 5 s of IQS (`HTRA_PORT` overrides port 5000).
- `examples/stream_test.c iqs|rta [seconds] [decimate]`: sustained streaming
  test. Reports the delivered rate, dropped packets (from the device
  timestamps), errors, temperature, and supply voltage/current before and after
  (`HTRA_POWER_DURING=1` also reads them every 5 s while streaming).

## Licence

This port is released under the [MIT licence](LICENSE). The Harogic SDK in
[`vendor/Linux_API`](vendor/README.md) is Harogic's and is included unchanged;
it is not covered by this project's licence.
