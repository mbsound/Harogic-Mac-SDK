# Build libhtraapi.dylib (arm64 macOS) from Harogic's aarch64 Linux SDK.
#
#   make SDK=path/to/Linux_API          # the unzipped Linux_API-x-y-z.zip
#   make dist SDK=path/to/Linux_API     # relocatable dist/htraapi-macos-arm64
#
# Requires Homebrew: gcc (for libstdc++), libusb, binutils.

SDK     ?= vendor/Linux_API
VENDOR  ?= $(SDK)/htraapi/lib/aarch64
VENDOR_INC  ?= $(SDK)/htraapi/inc
VENDOR_CONF ?= $(SDK)/htraapi/configs/htrausb.conf
BREW    ?= /opt/homebrew
GCCLIB  ?= $(BREW)/opt/gcc/lib/gcc/current

CC      := clang
CXX     := $(firstword $(wildcard $(BREW)/opt/gcc/bin/g++-[0-9]*))
CFLAGS  := -arch arm64 -O2 -g -fPIC -fexceptions -Wall -Wextra -Wno-unused-parameter \
           -mmacosx-version-min=14.4 -Isrc -I$(BREW)/include/libusb-1.0
CXXFLAGS := -O2 -g -fPIC -std=c++17 -Wall -Wno-invalid-offsetof -mmacosx-version-min=14.4 -Isrc
LDFLAGS := -arch arm64 -dynamiclib -mmacosx-version-min=14.4 \
           -install_name @rpath/libhtraapi.dylib \
           -L$(BREW)/lib -lusb-1.0 $(GCCLIB)/libstdc++.6.dylib -Wl,-force_load,$(GCCLIB)/libstdc++fs.a -Wl,-rpath,$(GCCLIB) \
           -Wl,-exported_symbols_list,src/gen/exports.txt -Wl,-exported_symbols_list,src/cxx_exports.txt

BUILD   := build
SRCS    := src/elf_loader.c src/htraapi_mac.c src/shim_libc.c src/shim_pthread.c \
           src/shim_cxx.c src/shim_net.c src/shim_usb.c
CXXSRCS := src/shim_fstream.cpp src/shim_alloc.cpp
ASMS    := src/shim_asm.S src/gen/exports.S src/gen/images.S
OBJS    := $(SRCS:src/%.c=$(BUILD)/%.o) $(CXXSRCS:src/%.cpp=$(BUILD)/%.o) $(ASMS:src/%.S=$(BUILD)/%.o)

all: check-sdk $(BUILD)/libhtraapi.dylib

check-sdk:
	@test -d "$(VENDOR)" || { echo "error: Harogic SDK not found at '$(SDK)'."; \
	  echo "Unzip Harogic's Linux_API-x-y-z.zip and run: make SDK=path/to/Linux_API"; exit 1; }

# Regenerate when the SDK location or its library files change. (Paths may
# contain spaces, so they are tracked through a stamp rather than as make
# prerequisites.)
SDK_STAMP := src/gen/.sdk-stamp

$(SDK_STAMP): FORCE
	@mkdir -p src/gen
	@new="$$(cd "$(VENDOR)" && pwd; ls -l "$(VENDOR)")"; \
	  [ -f $@ ] && [ "$$(cat $@)" = "$$new" ] || printf '%s\n' "$$new" > $@

src/gen/exports.S src/gen/images.S src/gen/images.h src/gen/exports.txt: tools/gen_build.py tools/x18_patch.py $(SDK_STAMP)
	python3 tools/gen_build.py "$(VENDOR)"

$(BUILD)/%.o: src/%.c src/gen/images.h src/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.cpp src/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S src/gen/images.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libhtraapi.dylib: $(OBJS) src/gen/exports.txt
	$(CC) $(LDFLAGS) $(OBJS) -o $@
	codesign -f -s - $@

EXAMPLES := $(BUILD)/swp_test $(BUILD)/mode_test $(BUILD)/stream_test

examples: $(EXAMPLES)

$(BUILD)/%: examples/%.c $(BUILD)/libhtraapi.dylib
	$(CC) -arch arm64 -O2 -g -Wno-comment -I"$(VENDOR_INC)" $< -L$(BUILD) -lhtraapi \
	  -Wl,-rpath,@executable_path -o $@

dist: $(BUILD)/libhtraapi.dylib
	sh tools/make_dist.sh $(BUILD) "$(VENDOR_INC)" "$(VENDOR_CONF)" dist/htraapi-macos-arm64

clean:
	rm -rf $(BUILD) src/gen dist

.PHONY: all check-sdk clean dist examples FORCE
FORCE:
