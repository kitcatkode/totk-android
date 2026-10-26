# Linux / Android build of the native runtime (headless: no window, null GPU backend).
#
#   make -C android TARGET=android NDK=/path/to/android-ndk  VERSION=v143  GEN=<dir>  OPUS=<opus prefix>
#   make -C android TARGET=linux   CC=aarch64-linux-gnu-gcc  ...            (arm64 Linux, e.g. under qemu-aarch64)
#
# GEN holds the headers generated from the player's own game files for the selected version (runtime_tables.h,
# romfs_config.h, shared_fonts_config.h), made from modules rewritten with DM_HOST_TP=tpidr_el0 (android/build.sh).
# OPUS / VPX are install prefixes (include/, lib/) of libopus and, optionally, libvpx built for the same target;
# without libvpx the game's own movie decoder runs (cutscenes abort, as on iOS).
TOTK := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
TARGET ?= android
VERSION ?= v0
GEN ?= $(TOTK)/build/android-$(VERSION)
OUT ?= $(GEN)
OPUS ?=
VPX ?=
API ?= 33
# GPU backend: vk (android/gfx/nvn_vk.c, Vulkan) or null (android/gfx/nvn_null.c, no GPU)
GPU ?= vk
# APP=1: the runtime as libtotk.so for the Android app (NativeActivity, runtime/frontend_android.c) instead of the
# totk-native executable; android/apk/build_apk.py packs it into an APK.
APP ?= 0

ifeq ($(TARGET),android)
  ifeq ($(NDK),)
    $(error set NDK to the Android NDK directory (r26 or newer))
  endif
  HOST_TAG := $(shell uname -s | tr A-Z a-z)-x86_64
  CC := $(NDK)/toolchains/llvm/prebuilt/$(HOST_TAG)/bin/clang --target=aarch64-linux-android$(API)
  CXX := $(NDK)/toolchains/llvm/prebuilt/$(HOST_TAG)/bin/clang++ --target=aarch64-linux-android$(API)
  PLATFORM_LIBS := -laaudio -llog -ldl -lm
  # CPU target for the runtime and NVN layer (not the game's own code). The Thor's cores (A510/A710/A715/X3) have
  # no SVE (Qualcomm disables it; /proc/cpuinfo), so never -mcpu=cortex-a715/x3: those enable SVE2 and the
  # autovectoriser emits it (SIGILL). This is their common feature set; it also inlines LSE atomics.
  #   CPU_FLAGS="-march=armv8.6-a+fp16+fp16fml+dotprod -mtune=cortex-a715"
  CPU_FLAGS ?=
else
  CC ?= aarch64-linux-gnu-gcc
  PLATFORM_LIBS := -lpthread -ldl -lm
endif
# objects per CPU_FLAGS value, so switching flags never links stale objects
ifneq ($(strip $(CPU_FLAGS)),)
  CPU_TAG := -cpu$(shell printf '%s' '$(CPU_FLAGS)' | cksum | cut -d' ' -f1)
endif

ifeq ($(VERSION),v0)
  VDEF :=
  PATCHES := runtime/game_patches.c
else ifeq ($(VERSION),v143)
  VDEF := -DTOTK_GAME_V143=1
  PATCHES := runtime/game_patches_v143.c
else
  VDEF := -DTOTK_GAME_$(shell echo $(VERSION) | tr a-z A-Z)=1
  PATCHES := runtime/game_patches.c
endif

RUNTIME_C := runtime/loader.c $(PATCHES) runtime/world_survey.c runtime/game_slot.c runtime/services.c runtime/romfs.c \
  runtime/threads.c runtime/hle_stack.c runtime/gfx_bridge.c runtime/account.c runtime/input.c runtime/hid_feedback.c \
  runtime/motion_fusion.c runtime/autoload.c runtime/gfx_probe.c runtime/memory_probe.c runtime/mem_scan.c runtime/gamedata_live.c runtime/shared_font.c runtime/ipc.c \
  runtime/audio_renderer.c runtime/audio_mixer.c runtime/audio_reverb.c runtime/audio_effects.c runtime/fs_host.c \
  runtime/opus_codec.c runtime/movie_host.c runtime/time_host.c runtime/applet_host.c runtime/swkbd_host.c \
  runtime/host_tp.c runtime/host_os.c $(if $(filter 1,$(APP)),runtime/frontend_android.c,runtime/frontend_headless.c)
RUNTIME_S := runtime/gate.S runtime/hle_stack.S runtime/thread_enter.S
# The NVN layer (portable C), without the Mac's text overlay (CoreGraphics) and system-load reader (sysctl).
ifeq ($(GPU),vk)
  BACKEND_C := $(TOTK)/android/gfx/nvn_vk.c $(TOTK)/android/gfx/vk_load.c $(TOTK)/android/gfx/vk_fmt.c $(TOTK)/android/gfx/vk_res.c \
    $(TOTK)/android/gfx/vk_shader.c $(TOTK)/android/gfx/vk_draw.c $(TOTK)/android/gfx/vk_fg.c
  BACKEND_DEF := -DGPU_LOG_PREFIX='"[nvn-vk] "' -DGPU_VK
else ifeq ($(GPU),null)
  BACKEND_C := $(TOTK)/android/gfx/nvn_null.c
  BACKEND_DEF :=
else
  $(error GPU must be vk or null)
endif
GFX_C := $(filter-out %/fps_overlay.c %/totk_sysload.c,$(wildcard $(TOTK)/third_party/d2r_gfx/gfx/nvn/*.c $(TOTK)/third_party/d2r_gfx/gfx/tex/*.c)) \
  $(BACKEND_C) $(TOTK)/android/gfx/nvn_stubs.c
GFX_S := $(TOTK)/third_party/d2r_gfx/gfx/nvn/nvn_thunks.S $(TOTK)/third_party/d2r_gfx/gfx/nvn/nvn_entry.S

ifeq ($(APP),1)
  PICFLAG := -fPIC -DTOTK_ANDROID_APP -Dmain=totk_host_main
  OBJNAME := obj-app$(CPU_TAG)
else
  PICFLAG := -fPIE
  OBJNAME := obj$(CPU_TAG)
endif
COMMON := -O2 -g $(CPU_FLAGS) $(PICFLAG) -fno-omit-frame-pointer -D_GNU_SOURCE $(VDEF) -I$(GEN) -I$(TOTK)/runtime \
  -I$(TOTK)/third_party/d2r_gfx/gfx/nvn -I$(TOTK)/third_party/d2r_gfx/gfx/nvn/include
ifneq ($(OPUS),)
  COMMON += -I$(OPUS)/include/opus -I$(OPUS)/include
  OPUS_LIB := $(OPUS)/lib/libopus.a
endif
ifneq ($(VPX),)
  COMMON += -I$(VPX)/include
  VPX_LIB := $(VPX)/lib/libvpx.a
endif
WARN := -std=gnu11 -Wall -Wno-format -Wno-misleading-indentation -Wno-unused-function
COMPAT := -I$(TOTK)/android/compat/include -include $(TOTK)/android/compat/include/darwin_compat.h -I$(TOTK)/third_party/d2r_gfx/host

OBJ := $(OUT)/$(OBJNAME)
RUNTIME_O := $(patsubst runtime/%,$(OBJ)/rt/%.o,$(RUNTIME_C) $(RUNTIME_S))
GFX_O := $(patsubst $(TOTK)/%,$(OBJ)/gfx/%.o,$(GFX_C) $(GFX_S))
# the app's second screen decodes the game's map tiles and icons (runtime/bottom_assets.c): zstd and ASTC, vendored
TP_C := $(if $(filter 1,$(APP)),$(TOTK)/third_party/zstd/zstddeclib.c $(TOTK)/third_party/astc/astc_c.cpp)
TP_O := $(patsubst $(TOTK)/third_party/%,$(OBJ)/tp/%.o,$(TP_C))

ifeq ($(APP),1)
all: $(OUT)/libtotk.so $(OUT)/libdl_android.so $(OUT)/liblsfg.so
else
all: $(OUT)/totk-native
endif

$(OBJ)/rt/%.c.o: $(TOTK)/runtime/%.c $(GEN)/runtime_tables.h
	@mkdir -p $(dir $@)
	$(CC) $(WARN) $(COMMON) -c $< -o $@
$(OBJ)/rt/%.S.o: $(TOTK)/runtime/%.S
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) -c $< -o $@
$(OBJ)/gfx/%.c.o: $(TOTK)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(WARN) -Wno-unused-parameter $(COMMON) $(COMPAT) $(BACKEND_DEF) -c $< -o $@
# the backend's own headers (vk_int.h declares the device function table and shared structs): a change rebuilds it all
$(filter $(OBJ)/gfx/android/%,$(GFX_O)): $(wildcard $(TOTK)/android/gfx/*.h)
$(OBJ)/tp/%.c.o: $(TOTK)/third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 $(PICFLAG) -w -DZSTD_DISABLE_ASM -I$(dir $<) -c $< -o $@
$(OBJ)/tp/%.cpp.o: $(TOTK)/third_party/%.cpp $(wildcard $(TOTK)/third_party/astc/*.h $(TOTK)/third_party/astc/*.cpp)
	@mkdir -p $(dir $@)
	$(CXX) -O2 $(PICFLAG) -w -fno-exceptions -fno-rtti -I$(dir $<) -c $< -o $@
$(OBJ)/gfx/%.S.o: $(TOTK)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) -c $< -o $@

# -rdynamic: the NVN layer and the host-stack wrappers name host functions with dladdr in diagnostics.
$(OUT)/totk-native: $(RUNTIME_O) $(GFX_O)
	$(CC) -pie -rdynamic -o $@ $^ $(OPUS_LIB) $(VPX_LIB) $(PLATFORM_LIBS)
	@echo "built $@ ($(TARGET), $(VERSION), GPU $(GPU))"

# libtotk.map: only ANativeActivity_onCreate is exported, so the runtime's symbols bind inside the library.
$(OUT)/libtotk.so: $(RUNTIME_O) $(GFX_O) $(TOTK)/android/libtotk.map $(TOTK)/runtime/bottom_screen.c $(TOTK)/runtime/bottom_inventory.c $(TOTK)/runtime/bottom_assets.c $(TOTK)/runtime/bottom_map.c $(TP_O)   # included by frontend_android.c, which this recipe compiles
	@rm -f $(OBJ)/rt/frontend_android.c.o; $(CC) $(WARN) $(COMMON) -c $(TOTK)/runtime/frontend_android.c -o $(OBJ)/rt/frontend_android.c.o   # the build time in the log is this link's
	$(CC) -shared -Wl,--version-script=$(TOTK)/android/libtotk.map -Wl,--no-undefined -Wl,-soname,libtotk.so -o $@ $(RUNTIME_O) $(GFX_O) $(TP_O) $(OPUS_LIB) $(VPX_LIB) $(PLATFORM_LIBS) -landroid
	@echo "built $@ ($(TARGET), $(VERSION), GPU $(GPU), app)"
# The app's libdl_android.so (android/apk/dl_android_stub.c): what the Vulkan driver's system dependencies get instead of
# the platform's linker-namespace API.
# No symbol versions: bionic accepts an unversioned definition for any version a library asks for.
$(OUT)/libdl_android.so: $(TOTK)/android/apk/dl_android_stub.c $(TOTK)/android/Makefile
	$(CC) -O2 -fPIC -shared -Wl,-soname,libdl_android.so -o $@ $<

# liblsfg.so: lsfg-vk's frame generation core (third_party/lsfg-vk, MIT; ORIGIN.md), loaded by android/gfx/vk_fg.c with
# dlopen when TOTK_LSFG is set. A library of its own: its volk defines the global vkXxx names the backend uses too.
LSFG := $(TOTK)/third_party/lsfg-vk
LSFG_CXX := $(wildcard $(LSFG)/framegen/src/*/*.cpp $(LSFG)/framegen/v3.1_src/*.cpp $(LSFG)/framegen/v3.1_src/*/*.cpp \
  $(LSFG)/framegen/v3.1p_src/*.cpp $(LSFG)/framegen/v3.1p_src/*/*.cpp) $(LSFG)/extract/dll.cpp $(LSFG)/lsfg_c.cpp
LSFG_O := $(patsubst $(LSFG)/%.cpp,$(OBJ)/lsfg/%.o,$(LSFG_CXX)) $(OBJ)/lsfg/volk.o
LSFG_FLAGS := -std=c++20 -O2 -g -fPIC -fvisibility=hidden -fexceptions -frtti -I$(LSFG)/framegen/include -I$(LSFG)/framegen/public \
  -I$(LSFG)/framegen/v3.1_include -I$(LSFG)/framegen/v3.1p_include -I$(LSFG)/framegen/include/thirdparty -I$(LSFG) -w
$(OBJ)/lsfg/%.o: $(LSFG)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(LSFG_FLAGS) -c $< -o $@
$(LSFG_O): $(wildcard $(LSFG)/lsfg_c.h $(LSFG)/extract/*.hpp $(LSFG)/framegen/public/*.hpp $(LSFG)/framegen/include/*/*.hpp \
  $(LSFG)/framegen/v3.1_include/*/*.hpp $(LSFG)/framegen/v3.1_include/*/*/*.hpp $(LSFG)/framegen/v3.1p_include/*/*.hpp $(LSFG)/framegen/v3.1p_include/*/*/*.hpp)
$(OBJ)/lsfg/volk.o: $(LSFG)/framegen/src/thirdparty/volk.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -fPIC -fvisibility=hidden -I$(LSFG)/framegen/include/thirdparty -c $< -o $@
$(OUT)/liblsfg.so: $(LSFG_O) $(LSFG)/liblsfg.map
	$(CXX) -shared -Wl,-soname,liblsfg.so -Wl,--no-undefined -Wl,--version-script=$(LSFG)/liblsfg.map -o $@ $(LSFG_O) -ldl -llog -static-libstdc++
	@echo "built $@"

# The backend's own compute shader (frame generation's HUD mask), kept as a generated header beside its source:
#   make -C android hud-spv NDK=/path/to/android-ndk
hud-spv:
	$(NDK)/shader-tools/$(HOST_TAG)/glslc --target-env=vulkan1.2 -O $(TOTK)/android/gfx/shaders/hud_mask.comp -o $(OUT)/hud_mask.spv
	python3 -c "import struct,sys; w=open(sys.argv[1],'rb').read(); v=struct.unpack('<%dI'%(len(w)//4),w); print('/* generated from hud_mask.comp: glslc --target-env=vulkan1.2 -O hud_mask.comp (android/Makefile hud-spv) */\nstatic const uint32_t hud_mask_spv[] = {'); [print('    '+', '.join('0x%08x'%x for x in v[i:i+8])+',') for i in range(0,len(v),8)]; print('};')" $(OUT)/hud_mask.spv > $(TOTK)/android/gfx/shaders/hud_mask.spv.h

# The Vulkan capability probe (android/vkprobe): needs no game files.
#   make -C android vkprobe NDK=/path/to/android-ndk
vkprobe: $(OUT)/vkprobe
$(OUT)/vkprobe: $(TOTK)/android/vkprobe/vkprobe.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -fPIE -pie -o $@ $< -ldl
	@echo "built $@"

# The Vulkan backend's own tests (no game files): android/gfx/test_vk.c (pools, GPU copies, ordering, events, aliases)
# and android/gfx/test_vkdraw.c (a textured draw through the whole backend; its shader seed comes from
# android/gfx/test_vkdraw_seed.py, which needs glslangValidator).
#   make -C android vktest NDK=/path/to/android-ndk OUT=/tmp/vk      ->  vktest, vkdrawtest and vkdraw-seed/ in OUT
VKTEST_SRC := $(TOTK)/android/gfx/nvn_vk.c $(TOTK)/android/gfx/vk_load.c $(TOTK)/android/gfx/vk_fmt.c $(TOTK)/android/gfx/vk_res.c \
  $(TOTK)/third_party/d2r_gfx/gfx/nvn/totk_timeline.c \
  $(TOTK)/android/gfx/vk_shader.c $(TOTK)/android/gfx/vk_draw.c $(TOTK)/third_party/d2r_gfx/gfx/nvn/nvn_tex.c \
  $(TOTK)/third_party/d2r_gfx/gfx/nvn/nvn_dirty.c $(TOTK)/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c $(TOTK)/android/gfx/host_test_support.c
VKTEST_FLAGS := -O1 -g $(WARN) -Wno-unused-parameter -D_GNU_SOURCE -DGPU_VK -I$(TOTK)/runtime -I$(TOTK)/third_party/d2r_gfx/gfx/nvn \
  -I$(TOTK)/third_party/d2r_gfx/gfx/nvn/include $(COMPAT) -fPIE -pie
VKTEST_LIBS := -ldl -lm $(if $(filter linux,$(TARGET)),-lpthread)
vktest: $(OUT)/vktest $(OUT)/vkdrawtest $(OUT)/vkdraw-seed/.done
$(OUT)/vktest: $(TOTK)/android/gfx/test_vk.c $(VKTEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -o $@ $^ $(VKTEST_LIBS)
	@echo "built $@"
$(OUT)/vkdrawtest: $(TOTK)/android/gfx/test_vkdraw.c $(VKTEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -o $@ $^ $(VKTEST_LIBS)
	@echo "built $@"
# The same draw test with the app's window presenter against a stand-in window (desktop only: TARGET=linux).
vktest-app: $(OUT)/vkdrawtest-app $(OUT)/vkdraw-seed/.done
$(OUT)/vkdrawtest-app: $(TOTK)/android/gfx/test_vkdraw.c $(TOTK)/android/gfx/test_window_mock.c $(TOTK)/android/gfx/vk_fg.c $(VKTEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(VKTEST_FLAGS) -DTOTK_ANDROID_APP -I$(TOTK)/android/gfx/test_shim -o $@ $^ $(VKTEST_LIBS)
	@echo "built $@"
$(OUT)/vkdraw-seed/.done: $(TOTK)/android/gfx/test_vkdraw_seed.py $(TOTK)/android/vk_reflect.py
	python3 $< $(OUT)/vkdraw-seed && touch $@

clean:
	rm -rf $(OUT)/obj $(OUT)/obj-app $(OUT)/totk-native $(OUT)/libtotk.so $(OUT)/vkprobe $(OUT)/vktest $(OUT)/vkdrawtest $(OUT)/vkdrawtest-app $(OUT)/vkdraw-seed
.PHONY: all clean vkprobe vktest vktest-app
