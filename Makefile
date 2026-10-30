# Top-level build. Run on the Ryzentosh (needs Xcode command line tools).
#
#   make test     host-side unit tests, works on any OS with a C compiler
#   make tools    build/rhv-info, build/rhv-guest-test, build/rhv-map-test, build/rhv-vmx-test, build/rhv-long-test, build/hvf-smoke
#   make shim     build/libryzenhv.dylib
#   make kext     build/RyzenHV.kext   (see kext/Makefile, best effort)
#   make all      tools + shim + kext
#   make clean

BUILD := build
CC    ?= cc
CXX   ?= c++

.PHONY: all test tools shim hijack hijack-patchonly kext clean

all: tools shim kext

$(BUILD):
	mkdir -p $(BUILD)

test: | $(BUILD)
	$(CC) -Wall -Wextra -Werror -o $(BUILD)/host_test tests/host_test.c
	$(BUILD)/host_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -o $(BUILD)/vmcb_layout tests/vmcb_layout.cpp
	$(BUILD)/vmcb_layout
	$(CXX) -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -o $(BUILD)/npt_test tests/npt_test.cpp
	$(BUILD)/npt_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -o $(BUILD)/vmx_map_test tests/vmx_map_test.cpp
	$(BUILD)/vmx_map_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -o $(BUILD)/vmcs_state_test tests/vmcs_state_test.cpp
	$(BUILD)/vmcs_state_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -mavx2 -fsanitize=address,undefined -g -o $(BUILD)/fpu_state_test tests/fpu_state_test.cpp
	$(BUILD)/fpu_state_test
	$(CXX) -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -o $(BUILD)/msr_map_test tests/msr_map_test.cpp
	$(BUILD)/msr_map_test
	$(CC) -Wall -Wextra -Werror -fsanitize=address,undefined -g -o $(BUILD)/patch_test tests/patch_test.c shim/rhv_patch.c
	$(BUILD)/patch_test
	$(CC) -arch x86_64 -Wall -Wextra -Werror -Wno-nullability-completeness -fsanitize=address,undefined -g -o $(BUILD)/shim_core_test tests/shim_core_test.c shim/rhv_core.c -framework Hypervisor
	$(BUILD)/shim_core_test

tools: | $(BUILD)
	$(CC) -arch x86_64 -Wall -Wextra -O2 -o $(BUILD)/rhv-info tools/rhv-info/main.c \
		-framework IOKit -framework CoreFoundation
	$(CC) -arch x86_64 -Wall -Wextra -O2 -o $(BUILD)/rhv-guest-test tools/rhv-guest-test/main.c \
		-framework IOKit -framework CoreFoundation
	$(CC) -arch x86_64 -Wall -Wextra -O2 -o $(BUILD)/rhv-map-test tools/rhv-map-test/main.c \
		-framework IOKit -framework CoreFoundation
	$(CC) -arch x86_64 -Wall -Wextra -O2 -o $(BUILD)/rhv-vmx-test tools/rhv-vmx-test/main.c \
		-framework IOKit -framework CoreFoundation
	$(CC) -arch x86_64 -Wall -Wextra -O2 -o $(BUILD)/rhv-long-test tools/rhv-long-test/main.c \
		-framework IOKit -framework CoreFoundation
	$(CC) -arch x86_64 -Wall -Wextra -Wno-nullability-completeness -O2 -o $(BUILD)/hvf-smoke tools/hvf-smoke/main.c \
		-framework Hypervisor

SHIM_SRC := shim/hv_shim.c shim/rhv_core.c shim/rhv_iokit.c shim/rhv_patch.c

shim: | $(BUILD)
	$(CC) -arch x86_64 -Wall -Wextra -Wno-deprecated-declarations -Wno-nullability-completeness -O2 -dynamiclib \
		-o $(BUILD)/libryzenhv.dylib $(SHIM_SRC) -framework Hypervisor -framework IOKit -framework CoreFoundation

# The shim as a stand-in for one of Parallels' own libraries (see shim/hijack.c and scripts/parallels-shim.sh).
# HIJACK_NAME is the library's file name, HIJACK_ORIG a copy of the real one with its install name changed to where the
# real one will be found at run time (only the linker reads it).
HIJACK_NAME ?= libPrlXmlModel.1.dylib
HIJACK_ORIG ?= $(BUILD)/hijack-link/$(HIJACK_NAME)
HIJACK_ORIG_RUNTIME ?= @loader_path/libPrlXmlModel.1.orig.dylib

hijack: | $(BUILD)
	@test -f "$(HIJACK_ORIG)" || { echo "run scripts/parallels-shim.sh prepare first (it makes $(HIJACK_ORIG))"; exit 1; }
	$(CC) -arch x86_64 -Wall -Wextra -Wno-deprecated-declarations -Wno-nullability-completeness -O2 -dynamiclib -DRHV_HIJACK \
		-install_name @rpath/$(HIJACK_NAME) -Wl,-reexport_library,$(HIJACK_ORIG) \
		-o $(BUILD)/$(HIJACK_NAME) $(SHIM_SRC) shim/hijack.c -framework Hypervisor -framework IOKit -framework CoreFoundation

# Native mode (rhv_hv=1): the same stand-in library, but it patches Parallels' vendor check and arms its keyboard (it wraps only hv_vcpu_run and hv_vcpu_run_until)
# (shim/hijack_patchonly.c). Needs the same build/hijack-link copy of the real library.
hijack-patchonly: | $(BUILD)
	@test -f "$(HIJACK_ORIG)" || { echo "run scripts/parallels-shim.sh prepare first (it makes $(HIJACK_ORIG))"; exit 1; }
	$(CC) -arch x86_64 -Wall -Wextra -O2 -dynamiclib \
		-install_name @rpath/$(HIJACK_NAME) -Wl,-reexport_library,$(HIJACK_ORIG) \
		-o $(BUILD)/$(HIJACK_NAME) shim/hijack_patchonly.c shim/rhv_patch.c

kext:
	$(MAKE) -C kext

clean:
	rm -rf $(BUILD)
