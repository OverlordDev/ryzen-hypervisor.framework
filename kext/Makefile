# Command-line build of RyzenHV.kext, NO Xcode project needed.
#
# STATUS: best effort, not verified on a real Sequoia SDK yet. If it fails,
# the two usual suspects are the library search path for -lkmod/-lkmodc++/
# -lcc_kext and missing Kernel.framework headers. Paste me the error, or
# fall back to an Xcode kext project (copy the build settings of VoodooTSCSync).

SDK     := $(shell xcrun --sdk macosx --show-sdk-path)
KFW     := $(SDK)/System/Library/Frameworks/Kernel.framework
OUT     := ../build
KEXT    := $(OUT)/RyzenHV.kext
OBJDIR  := $(OUT)/kext-obj

CXXSRC  := $(wildcard Source/*.cpp)
CSRC    := $(wildcard Source/*.c)
ASMSRC  := $(wildcard Source/*.S)
OBJS    := $(patsubst Source/%.cpp,$(OBJDIR)/%.o,$(CXXSRC)) \
           $(patsubst Source/%.c,$(OBJDIR)/%.o,$(CSRC)) \
           $(patsubst Source/%.S,$(OBJDIR)/%.o,$(ASMSRC))

# make DEBUG=1  ->  -O0 -g, RHV_DEBUG defined (kernel log level defaults to DEBUG)
DEBUG   ?= 0
ifeq ($(DEBUG),1)
OPT     := -O0 -g -DRHV_DEBUG=1
else
OPT     := -O2
endif

COMMON  := -arch x86_64 -isysroot $(SDK) -mmacosx-version-min=10.15 \
           -mkernel -nostdinc -nostdlibinc \
           -I$(KFW)/Headers -I$(KFW)/PrivateHeaders \
           -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
           -Wall -Wextra $(OPT)

CXXFLAGS := $(COMMON) -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti -fno-builtin
CFLAGS   := $(COMMON) -fno-builtin

.PHONY: all clean

all: $(KEXT)

$(OBJDIR)/%.o: Source/%.cpp
	@mkdir -p $(OBJDIR)
	xcrun clang++ $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/%.o: Source/%.c
	@mkdir -p $(OBJDIR)
	xcrun clang $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: Source/%.S
	@mkdir -p $(OBJDIR)
	xcrun clang -arch x86_64 -isysroot $(SDK) -mmacosx-version-min=10.15 -c $< -o $@

$(KEXT): $(OBJS) Info.plist
	rm -rf $(KEXT)
	mkdir -p $(KEXT)/Contents/MacOS
	cp Info.plist $(KEXT)/Contents/Info.plist
	xcrun clang++ -arch x86_64 -isysroot $(SDK) -mkernel -nostdlib -fapple-kext \
		-Xlinker -kext -L$(SDK)/usr/lib \
		-o $(KEXT)/Contents/MacOS/RyzenHV $(OBJS) \
		-lkmodc++ -lkmod -lcc_kext
	../scripts/check-kext-symbols.sh $(KEXT)
	@echo "Built $(KEXT)"
	@echo "OpenCore loads it unsigned via Kernel -> Add, see docs/RYZENTOSH.md"

clean:
	rm -rf $(KEXT) $(OBJDIR)
