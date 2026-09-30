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
| `HTRAAPI_USB_READAHEAD=n` | Streaming read-ahead queue depth (default 16; 0 disables). |

## Status (SAN-60, SDK 0.55.89)

| Mode | Result |
|---|---|
| Device open / info / state / close, calibration load from files or flash | ✅ |
| SWP: 1000 full 50 MHz–6 GHz sweeps at ~87 sweeps/s, zero errors | ✅ |
| RTA, IQS (+ DSP FFT/DDC), DET, MSCAN | ✅ at ≤31 MS/s |
| IQS/RTA at the top rate (62.5 MS/s, ~250 MB/s) | ⚠️ the analyzer drops off USB. See below. |
| `libDigitalSigDemod` plugin | ✅ loads; `Demod_Open` needs Harogic's demod licence file (-60 otherwise, same as Linux) |
| PCIe devices | ❌ Linux kernel driver only |
| Ethernet devices (NX series) | Shimmed but untested |

**Top-rate streaming.** At 62.5 MS/s, and at 31 MS/s after sustained activity,
the analyzer disconnects from the bus (`kIOReturnNotResponding`, then a hotplug
detach). It still happens with 3 MB of USB reads queued ahead, so host latency
is ruled out. The pattern points to power or USB topology (a bus-powered hub).
Try a direct Mac port and the analyzer's external power supply.

## Examples

Build them with `make examples`:

- `examples/swp_test.c`: open, identify, sweep, report peaks.
- `examples/mode_test.c [state swp rta iqs det mscan]`: exercises every mode.
  Set `HTRA_DECIM` for IQS/RTA decimation and `HTRA_SWEEPS` for the SWP stress
  count.

## Licence

This port is released under the [MIT licence](LICENSE). The Harogic SDK in
[`vendor/Linux_API`](vendor/README.md) is Harogic's and is included unchanged;
it is not covered by this project's licence.
