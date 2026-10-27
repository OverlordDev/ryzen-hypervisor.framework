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

.PHONY: all test tools shim kext clean

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

SHIM_SRC := shim/hv_shim.c shim/rhv_core.c shim/rhv_iokit.c

shim: | $(BUILD)
	$(CC) -arch x86_64 -Wall -Wextra -Wno-deprecated-declarations -Wno-nullability-completeness -O2 -dynamiclib \
		-o $(BUILD)/libryzenhv.dylib $(SHIM_SRC) -framework Hypervisor -framework IOKit -framework CoreFoundation

kext:
	$(MAKE) -C kext

clean:
	rm -rf $(BUILD)
