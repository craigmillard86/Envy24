# Builds Envy24PCIAudioDriver.kext with the Command Line Tools (no Xcode needed).
# x86_64 only, unsigned.

SDK      ?= $(shell xcrun --sdk macosx14.5 --show-sdk-path 2>/dev/null || xcrun --sdk macosx --show-sdk-path)
MINOS    := 14.0
KEXT     := build/Envy24PCIAudioDriver.kext
EXEC     := Envy24PCIAudioDriver
BUNDLEID := com.audio-evolution.driver.Envy24

KERNHDR  := $(SDK)/System/Library/Frameworks/Kernel.framework/Headers

KEXT_SRCS := AudioDevice.cpp AudioEngine.cpp I2C.cpp misc.cpp ak_codec.cpp Phase88.cpp Delta66.cpp
LIB_SRCS  := SampleAudioClip.cpp

OBJS := $(addprefix build/obj/,$(KEXT_SRCS:.cpp=.o) $(LIB_SRCS:.cpp=.o) kmod_info.o)

COMMON := -arch x86_64 -mmacosx-version-min=$(MINOS) -isysroot $(SDK) \
          -mkernel -fapple-kext -nostdinc -fno-builtin -fno-common \
          -isystem $(KERNHDR) -Idriver \
          -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
          -O2 -g -Wall -Wno-four-char-constants -Wno-unknown-pragmas \
          -Wno-deprecated-declarations -Wno-#warnings \
          $(EXTRA_CFLAGS)
CXXFLAGS := $(COMMON) -std=gnu++17 -fno-exceptions -fno-rtti
CFLAGS   := $(COMMON) -std=gnu11

LDFLAGS := -arch x86_64 -mmacosx-version-min=$(MINOS) -isysroot $(SDK) \
           -mkernel -fapple-kext -nostdlib -Xlinker -kext \
           -Xlinker -export_dynamic -L$(SDK)/usr/lib \
           -lkmodc++ -lkmod -lcc_kext

all: $(KEXT)

build/obj/%.o: driver/%.cpp | build/obj
	clang++ $(CXXFLAGS) -c $< -o $@

build/obj/kmod_info.o: build-support/kmod_info.c | build/obj
	clang $(CFLAGS) -c $< -o $@

build/obj:
	mkdir -p $@

$(KEXT): $(OBJS) driver/Info-Envy24AudioDriver__Upgraded_.plist
	rm -rf $@
	mkdir -p $@/Contents/MacOS
	clang++ $(LDFLAGS) $(OBJS) -o $@/Contents/MacOS/$(EXEC)
	cp driver/Info-Envy24AudioDriver__Upgraded_.plist $@/Contents/Info.plist
	plutil -lint $@/Contents/Info.plist

check: $(KEXT)
	kextutil -n -t $(KEXT) || kextlibs -xml $(KEXT)

clean:
	rm -rf build

.PHONY: all check clean
