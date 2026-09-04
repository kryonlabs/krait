CC ?= cc
BUILD_DIR ?= build/$(KRYON_PLATFORM)-$(KRYON_ARCH)
SITE_DIR ?= docs/site
SITE_BUILD_DIR ?= build/site
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
INSTALL ?= install
KRYON_DIR ?= vendor/kryon
DEV_KRYON_DIR ?= ../kryon
# Kapsule terminal emulator, embedded as the studio console tab. Only the
# emulator core is compiled (src/native_console.c drives it); the Kapsule
# product UI stays out of the build.
KAPSULE_DIR ?= vendor/kapsule
KRYON_UNAME_S := $(shell uname -s 2>/dev/null)
KRYON_UNAME_M := $(shell uname -m 2>/dev/null)
ifeq ($(KRYON_UNAME_M),amd64)
    KRYON_ARCH := x86_64
else
    KRYON_ARCH := $(KRYON_UNAME_M)
endif
ifeq ($(KRYON_UNAME_S),Linux)
    KRYON_PLATFORM := linux
else ifeq ($(KRYON_UNAME_S),FreeBSD)
    KRYON_PLATFORM := freebsd
else ifeq ($(KRYON_UNAME_S),Darwin)
    KRYON_PLATFORM := macos
else
    KRYON_PLATFORM := $(KRYON_UNAME_S)
endif
KRYON_BUILD_DIR ?= $(KRYON_DIR)/build/$(KRYON_PLATFORM)-$(KRYON_ARCH)

KRAIT = $(BUILD_DIR)/bin/krait
LEVEL = $(BUILD_DIR)/bin/level
KRAIT_GEN = $(BUILD_DIR)/gen
KRAIT_SRCS := $(wildcard ide/*.kry) $(wildcard modules/*/*.kry)
KRAIT_OBJS := $(patsubst %.kry,$(KRAIT_GEN)/%.o,$(KRAIT_SRCS))
KRAIT_NATIVE_SRCS := src/main.c $(wildcard src/native_*.c)
KRAIT_NATIVE_OBJS := $(patsubst src/%.c,$(BUILD_DIR)/src/%.o,$(KRAIT_NATIVE_SRCS))
# Kapsule emulator core (vendored). Keep this list in sync with
# scripts/build-android-native.sh.
KAPSULE_EMULATOR_SRCS := terminal.c terminal_csi.c terminal_dcs.c \
	terminal_keys.c terminal_modes.c terminal_mouse.c terminal_osc.c \
	terminal_parser.c terminal_paste.c terminal_pty.c terminal_screen.c \
	terminal_search.c terminal_sgr.c terminal_sixel.c terminal_text.c \
	terminal_view.c session.c selection.c input.c palette.c
KAPSULE_OBJS := $(addprefix $(BUILD_DIR)/kapsule/,$(KAPSULE_EMULATOR_SRCS:.c=.o))
# kryon's daochi/ksync layer (account keys, login, bearer transport) is
# compiled as app sources like inbe does, not through libkryon.
KSYNC_SRCS := $(wildcard $(KRYON_DIR)/src/ksync/*.c)
KSYNC_OBJS := $(patsubst $(KRYON_DIR)/src/ksync/%.c,$(BUILD_DIR)/ksync/%.o,$(KSYNC_SRCS))

# Web build (kryon Canvas2D backend, emscripten). Compiles the krait natives,
# the k2c-generated C, and kryon's canvas sources with web liboqs for daochi
# accounts. Desktop-only surfaces (terminal, 3D, physics nodes) are either
# stubbed (web_stubs.c, native_console gates) or degrade to no-ops.
WEB_EMSDK_BIN ?= $(HOME)/emsdk/upstream/emscripten
WEB_CC ?= $(if $(wildcard $(WEB_EMSDK_BIN)/emcc),$(WEB_EMSDK_BIN)/emcc,emcc)
WEB_DIST_DIR := $(BUILD_DIR)/web
WEB_LIBOQS_A := $(KRYON_DIR)/build/web-liboqs/lib/liboqs.a
WEB_EMBEDDED_ASSETS_C := $(KRYON_BUILD_DIR)/embedded_asset_data.c
WEB_KRYON_SRCS := $(filter-out \
	$(KRYON_DIR)/src/backend/dom_%.c \
	$(KRYON_DIR)/src/backend/libdraw_%.c \
	$(KRYON_DIR)/src/backend/termi_%.c \
	$(KRYON_DIR)/src/platform/plan9/%.c \
	$(KRYON_DIR)/src/scene/node_body2d.c \
	$(KRYON_DIR)/src/scene/node_collision_shape2d.c \
	$(KRYON_DIR)/src/scene/physics_world.c \
	,$(shell find $(KRYON_DIR)/src -name '*.c' | LC_ALL=C sort))
WEB_STUB_C := src/web_stubs.c
WEB_DEBUG ?= 0
WEB_DEBUG_FLAGS := $(if $(filter 1,$(WEB_DEBUG)),-O0 -g3 -sASSERTIONS=1 -g3,)



K2C = $(KRYON_BUILD_DIR)/bin/k2c
# Detect a k2c built for the wrong platform (e.g. a FreeBSD binary on Linux):
# the kernel refuses to exec it, so the shell returns 126/127. When that
# happens, delete the stale binary so make's rule rebuilds it for the host.
K2C_RUNNABLE := $(shell if [ ! -x "$(K2C)" ]; then echo yes; \
    elif "$(K2C)" >/dev/null 2>&1; then echo yes; \
    else rc=$$?; if [ $$rc -eq 126 ] || [ $$rc -eq 127 ]; then echo no; else echo yes; fi; fi)
KRYON_LIB = $(KRYON_BUILD_DIR)/libkryon.a
RAYLIB_A = $(KRYON_BUILD_DIR)/raylib/libraylib.a
KRYON_LIBOQS_A = $(KRYON_BUILD_DIR)/vendor/liboqs/lib/liboqs.a
KRYON_CURL_A = $(KRYON_BUILD_DIR)/vendor/curl/lib/libcurl.a
KRYON_CMARK_GFM_A = $(KRYON_BUILD_DIR)/vendor/cmark-gfm/src/libcmark-gfm.a
KRYON_CMARK_GFM_EXTENSIONS_A = $(KRYON_BUILD_DIR)/vendor/cmark-gfm/extensions/libcmark-gfm-extensions.a
KRYON_BOX2D_A = $(KRYON_BUILD_DIR)/vendor/box2d/src/libbox2d.a

RAY_SDL_CFLAGS ?= $(shell pkg-config --cflags sdl2 2>/dev/null)
RAY_SDL_LDLIBS ?= $(shell pkg-config --libs sdl2 2>/dev/null)
RAY_GL_CFLAGS ?= $(shell pkg-config --cflags libdrm gbm egl glesv2 2>/dev/null)
RAY_GL_LDLIBS ?= $(shell pkg-config --libs libdrm gbm egl glesv2 2>/dev/null)
RAY_CFLAGS ?= $(strip $(RAY_SDL_CFLAGS) $(RAY_GL_CFLAGS))
RAY_LDLIBS ?= $(strip $(RAY_SDL_LDLIBS) $(RAY_GL_LDLIBS))
SYSTEM_THEME_PKG := $(shell if pkg-config --exists gtk+-3.0 2>/dev/null; then printf '%s' gtk+-3.0; fi)
ifneq ($(strip $(SYSTEM_THEME_PKG)),)
SYSTEM_THEME_CFLAGS := $(shell pkg-config --cflags $(SYSTEM_THEME_PKG))
SYSTEM_THEME_LDLIBS := $(shell pkg-config --libs $(SYSTEM_THEME_PKG))
else
SYSTEM_THEME_CFLAGS :=
SYSTEM_THEME_LDLIBS :=
endif
CURL_CODEC_LDLIBS ?= $(strip \
  $(shell pkg-config --libs libbrotlidec 2>/dev/null) \
  $(shell pkg-config --libs libbrotlicommon 2>/dev/null) \
  $(shell pkg-config --libs libzstd 2>/dev/null))
KRYON_OPENSSL_SSL_LDLIB ?= -lssl
KRYON_OPENSSL_CRYPTO_LDLIB ?= -lcrypto
KRYON_CURL_LDLIBS ?= $(KRYON_CURL_A) $(KRYON_OPENSSL_SSL_LDLIB) $(KRYON_OPENSSL_CRYPTO_LDLIB) -lpthread
KRYON_MARKDOWN_LDLIBS ?= $(KRYON_CMARK_GFM_EXTENSIONS_A) $(KRYON_CMARK_GFM_A)

CFLAGS ?= -Wall -Wextra -O2
CPPFLAGS += -I$(KAPSULE_DIR)/src -I$(KRYON_DIR)/include -I$(KRYON_DIR)/src/ui -I$(KRYON_DIR)/vendor/clay \
	$(RAY_SDL_CFLAGS) \
	-DHAS_LIBOQS=1 -I$(KRYON_BUILD_DIR)/vendor/liboqs/include \
	-DHAS_LIBCURL=1 -DCURL_STATICLIB -I$(KRYON_BUILD_DIR)/vendor/curl/include \
	-DKRYON_HAS_CMARK_GFM=1 \
	-I$(KRYON_DIR)/vendor/cmark-gfm/src -I$(KRYON_DIR)/vendor/cmark-gfm/extensions \
	-I$(KRYON_BUILD_DIR)/vendor/cmark-gfm/src -I$(KRYON_BUILD_DIR)/vendor/cmark-gfm/extensions
ifeq ($(KRYON_PLATFORM),linux)
KRYON_PLATFORM_LDLIBS ?= -ldl -lrt
else
KRYON_PLATFORM_LDLIBS ?=
endif

.PHONY: all krait level web web-serve run dev test smoke kanban-test daochi-test git-test engine-test clean install uninstall kryon-deps boundary-check docs-site appimage android-debug android-install android-clean

all: krait

krait: $(KRAIT)

kryon-deps:
	@if git -C "$(KRYON_DIR)" rev-parse --is-inside-work-tree >/dev/null 2>&1; then \
	    git -C "$(KRYON_DIR)" submodule update --init --recursive; \
	fi
	$(MAKE) -C $(KRYON_DIR) all

# Phony guard: if k2c exists but is built for a different platform (the kernel
# refuses to exec it), delete it so the $(K2C) rule below rebuilds it for the host.
.PHONY: ensure-k2c-runnable
ensure-k2c-runnable:
	@if [ "$(K2C_RUNNABLE)" != yes ] && [ -x "$(K2C)" ]; then \
	    echo "k2c at $(K2C) is not runnable on $(KRYON_PLATFORM); rebuilding"; \
	    rm -f "$(K2C)"; \
	fi

$(KRAIT_GEN)/.transpiled: $(KRAIT_SRCS) | ensure-k2c-runnable $(K2C)
	@mkdir -p $(KRAIT_GEN)
	$(K2C) --root . -o $(KRAIT_GEN) $(KRAIT_SRCS)
	@touch $@

$(KRAIT_GEN)/ide/app.o: $(KRAIT_GEN)/.transpiled
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -Dmain=krait_generated_main -I$(KRAIT_GEN) -c $(KRAIT_GEN)/ide/app.c -o $@

$(KRAIT_GEN)/ide/%.o: $(KRAIT_GEN)/.transpiled
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -I$(KRAIT_GEN) -c $(KRAIT_GEN)/ide/$*.c -o $@

$(KRAIT_GEN)/modules/%.o: $(KRAIT_GEN)/.transpiled
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -I$(KRAIT_GEN) -c $(KRAIT_GEN)/modules/$*.c -o $@

$(BUILD_DIR)/src/%.o: src/%.c $(KRAIT_GEN)/.transpiled
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -I$(KRAIT_GEN) -c $< -o $@

$(BUILD_DIR)/kapsule/%.o: $(KAPSULE_DIR)/src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/ksync/%.o: $(KRYON_DIR)/src/ksync/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(KRAIT): kryon-deps $(KRAIT_OBJS) $(KRAIT_NATIVE_OBJS) $(KSYNC_OBJS) $(KAPSULE_OBJS) $(KRAIT_GEN)/.transpiled $(KRYON_LIB) $(RAYLIB_A) $(KRYON_BOX2D_A) $(KRYON_LIBOQS_A) $(KRYON_CURL_A) $(KRYON_CMARK_GFM_A) $(KRYON_CMARK_GFM_EXTENSIONS_A) | $(BUILD_DIR)/bin
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(KRAIT_GEN) $(RAY_CFLAGS) $(SYSTEM_THEME_CFLAGS) -o $@ \
		$(KRAIT_OBJS) $(KRAIT_NATIVE_OBJS) $(KSYNC_OBJS) $(KAPSULE_OBJS) \
		-Wl,--whole-archive $(KRYON_LIB) -Wl,--no-whole-archive \
		$(RAYLIB_A) $(KRYON_BOX2D_A) $(RAY_LDLIBS) $(KRYON_LIBOQS_A) $(KRYON_CURL_LDLIBS) \
		$(KRYON_MARKDOWN_LDLIBS) \
		-Wl,-export-dynamic $(KRYON_PLATFORM_LDLIBS) \
		$(SYSTEM_THEME_LDLIBS) $(CURL_CODEC_LDLIBS) -lz -lpthread -lm

$(K2C) $(KRYON_LIB) $(KRYON_LIBOQS_A) $(KRYON_CURL_A) $(KRYON_CMARK_GFM_A) $(KRYON_CMARK_GFM_EXTENSIONS_A) $(KRYON_BOX2D_A):
	$(MAKE) -C $(KRYON_DIR) all

$(RAYLIB_A):
	$(MAKE) -C $(KRYON_DIR) $(patsubst $(KRYON_DIR)/%,%,$@)

$(BUILD_DIR)/bin:
	mkdir -p $@

$(WEB_DIST_DIR):
	mkdir -p $@

web: $(WEB_DIST_DIR)/index.html

$(WEB_LIBOQS_A): $(KRYON_DIR)/vendor/liboqs/CMakeLists.txt
	PATH="$(WEB_EMSDK_BIN):$$PATH" emcmake cmake -S $(KRYON_DIR)/vendor/liboqs -B $(KRYON_DIR)/build/web-liboqs \
		-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DOQS_BUILD_ONLY_LIB=ON \
		-DOQS_USE_OPENSSL=OFF -DOQS_DIST_BUILD=OFF -DOQS_OPT_TARGET=generic \
		-DOQS_MINIMAL_BUILD=SIG_ml_dsa_44
	cmake --build $(KRYON_DIR)/build/web-liboqs --target oqs

$(WEB_DIST_DIR)/krait.js: GNUmakefile $(KRAIT_NATIVE_SRCS) $(WEB_STUB_C) $(wildcard ide/*.kry) $(KRAIT_GEN)/.transpiled $(WEB_KRYON_SRCS) $(WEB_LIBOQS_A) $(WEB_EMBEDDED_ASSETS_C) | $(WEB_DIST_DIR)
	$(WEB_CC) $(filter -O%,$(CFLAGS)) $(WEB_DEBUG_FLAGS) -std=gnu99 \
		-I$(KRYON_DIR)/include -I$(KRYON_DIR)/src/ui -I$(KRYON_DIR)/vendor/clay \
		-I$(KRYON_BUILD_DIR)/generated -I$(KRAIT_GEN) -Isrc -I$(KAPSULE_DIR)/src \
		-I$(KRYON_DIR)/build/web-liboqs/include \
		-DKRYON_BACKEND_CANVAS=1 -DPLATFORM_WEB=1 -DKRYON_WITH_PHYSICS=0 -DHAS_LIBOQS=1 \
		-sASYNCIFY -sASYNCIFY_STACK_SIZE=1048576 -fexceptions \
		-sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=268435456 -sSTACK_SIZE=33554432 \
		-sEXPORTED_RUNTIME_METHODS=Asyncify -sEXPORTED_FUNCTIONS=_main,_malloc,_free \
		-lidbfs.js -sFORCE_FILESYSTEM=1 -sFETCH=1 \
		-o $@ \
		$(KRAIT_GEN)/ide/app.c $(filter-out $(KRAIT_GEN)/ide/app.c,$(shell find $(KRAIT_GEN) -name '*.c' | LC_ALL=C sort)) \
		$(KRAIT_NATIVE_SRCS) $(WEB_STUB_C) \
		$(WEB_KRYON_SRCS) $(WEB_EMBEDDED_ASSETS_C) $(WEB_LIBOQS_A)

$(WEB_DIST_DIR)/index.html: web/index.html web/index_boot.js $(WEB_DIST_DIR)/krait.js | $(WEB_DIST_DIR)
	cp web/index.html web/index_boot.js $(WEB_DIST_DIR)/

web-serve: web
	cd $(WEB_DIST_DIR) && python3 -m http.server 8901 --bind 127.0.0.1

run: krait
	@kryon_dir=$$(cd "$(KRYON_DIR)" && pwd); \
	KRYON_DIR="$$kryon_dir" $(KRAIT) $(ARGS)

dev: KRYON_DIR := $(DEV_KRYON_DIR)
dev: krait
	@kryon_dir=$$(cd "$(KRYON_DIR)" && pwd); \
	KRYON_DIR="$$kryon_dir" $(KRAIT) --kryon-dir "$$kryon_dir" $(ARGS)

# Android (arm64-v8a). Builds the native libkrait.so (raylib+kryon+krait, cross
# via the NDK) and packages a signed, zipaligned debug APK. Requires the NDK and
# a platform jar; the scripts auto-detect ANDROID_NDK_HOME / ANDROID_HOME.
android-debug: krait
	ANDROID_API=$${ANDROID_API:-29} sh scripts/build-android-native.sh
	sh scripts/package-android-apk.sh

android-install: android-debug
	@adb=$$(command -v adb || echo "$${ANDROID_HOME:-$$HOME/Android/Sdk}/platform-tools/adb"); \
	$$adb install -r build/android-arm64/krait.apk && \
	$$adb shell am start -n com.kryonlabs.krait/android.app.NativeActivity

android-clean:
	rm -rf build/android-arm64

KANBAN_TEST = $(BUILD_DIR)/tests/kanban_test
DAOCHI_TEST = $(BUILD_DIR)/tests/daochi_test
GIT_TEST = $(BUILD_DIR)/tests/git_test
AGENT_TEST = $(BUILD_DIR)/tests/agent_test
ENGINE_TEST = $(BUILD_DIR)/tests/engine_test

test: krait boundary-check kanban-test daochi-test git-test agent-test engine-test

daochi-test: $(DAOCHI_TEST)
	$(DAOCHI_TEST)

git-test: $(GIT_TEST)
	$(GIT_TEST)

$(GIT_TEST): tests/git_test.c $(BUILD_DIR)/src/native_git.o $(BUILD_DIR)/src/native_kanban.o $(BUILD_DIR)/src/native_ai.o $(BUILD_DIR)/src/native_util.o $(BUILD_DIR)/src/native_scaffold.o $(BUILD_DIR)/src/native_agent.o $(BUILD_DIR)/src/native_project.o $(BUILD_DIR)/src/native_krbhex.o $(BUILD_DIR)/src/native_compile.o $(BUILD_DIR)/src/native_preview.o $(BUILD_DIR)/src/native_live.o $(BUILD_DIR)/src/native_live_eval.o $(BUILD_DIR)/src/native_scene.o $(KRYON_LIB) $(RAYLIB_A) $(KRYON_CURL_A) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -I$(KRAIT_GEN) -I$(KRYON_DIR)/include -o $@ tests/git_test.c \
		$(BUILD_DIR)/src/native_git.o $(BUILD_DIR)/src/native_kanban.o \
		$(BUILD_DIR)/src/native_ai.o $(BUILD_DIR)/src/native_util.o \
		$(BUILD_DIR)/src/native_scaffold.o $(BUILD_DIR)/src/native_agent.o \
		$(BUILD_DIR)/src/native_project.o $(BUILD_DIR)/src/native_krbhex.o \
		$(BUILD_DIR)/src/native_compile.o $(BUILD_DIR)/src/native_preview.o \
		$(BUILD_DIR)/src/native_live.o $(BUILD_DIR)/src/native_live_eval.o \
		$(BUILD_DIR)/src/native_scene.o \
		-Wl,--whole-archive $(KRYON_LIB) -Wl,--no-whole-archive \
		$(RAYLIB_A) $(KRYON_BOX2D_A) $(KRYON_LIBOQS_A) \
		$(KRYON_CURL_LDLIBS) $(KRYON_MARKDOWN_LDLIBS) $(RAY_LDLIBS) \
		$(SYSTEM_THEME_LDLIBS) $(CURL_CODEC_LDLIBS) \
		-lbrotlidec -lbrotlicommon -lzstd -lz -lpthread -lm

$(DAOCHI_TEST): tests/daochi_test.c $(BUILD_DIR)/src/native_daochi.o $(BUILD_DIR)/src/native_util.o $(KSYNC_OBJS) $(KRYON_LIB) $(RAYLIB_A) $(KRYON_LIBOQS_A) $(KRYON_CURL_A) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -I$(KRAIT_GEN) -I$(KRYON_DIR)/include -o $@ tests/daochi_test.c \
		$(BUILD_DIR)/src/native_daochi.o $(BUILD_DIR)/src/native_util.o $(KSYNC_OBJS) \
		-Wl,--whole-archive $(KRYON_LIB) -Wl,--no-whole-archive \
		$(RAYLIB_A) $(KRYON_BOX2D_A) $(KRYON_LIBOQS_A) \
		$(KRYON_CURL_LDLIBS) $(KRYON_MARKDOWN_LDLIBS) $(RAY_LDLIBS) \
		$(SYSTEM_THEME_LDLIBS) $(CURL_CODEC_LDLIBS) \
		-lbrotlidec -lbrotlicommon -lzstd -lz -lpthread -lm

kanban-test: $(KANBAN_TEST)
	$(KANBAN_TEST)

$(KANBAN_TEST): tests/kanban_test.c $(BUILD_DIR)/src/native_kanban.o $(BUILD_DIR)/src/native_ai.o $(BUILD_DIR)/src/native_util.o $(BUILD_DIR)/src/native_scaffold.o $(BUILD_DIR)/src/native_compile.o $(BUILD_DIR)/src/native_agent.o $(BUILD_DIR)/src/native_project.o $(BUILD_DIR)/src/native_preview.o $(BUILD_DIR)/src/native_live.o $(BUILD_DIR)/src/native_live_eval.o $(BUILD_DIR)/src/native_scene.o $(KRYON_LIB) $(RAYLIB_A) $(KRYON_CURL_A) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -I$(KRAIT_GEN) -I$(KRYON_DIR)/include -o $@ tests/kanban_test.c \
		$(BUILD_DIR)/src/native_kanban.o $(BUILD_DIR)/src/native_ai.o \
		$(BUILD_DIR)/src/native_util.o $(BUILD_DIR)/src/native_scaffold.o \
		$(BUILD_DIR)/src/native_compile.o $(BUILD_DIR)/src/native_agent.o \
		$(BUILD_DIR)/src/native_project.o $(BUILD_DIR)/src/native_preview.o \
		$(BUILD_DIR)/src/native_live.o $(BUILD_DIR)/src/native_live_eval.o \
		$(BUILD_DIR)/src/native_scene.o $(BUILD_DIR)/src/native_krbhex.o \
		-Wl,--whole-archive $(KRYON_LIB) -Wl,--no-whole-archive \
		$(RAYLIB_A) $(KRYON_BOX2D_A) $(KRYON_LIBOQS_A) \
		$(KRYON_CURL_LDLIBS) $(KRYON_MARKDOWN_LDLIBS) $(RAY_LDLIBS) \
		$(SYSTEM_THEME_LDLIBS) $(CURL_CODEC_LDLIBS) \
			-lbrotlidec -lbrotlicommon -lzstd -lz -lpthread -lm

BENCH_TEST = $(BUILD_DIR)/tests/bench_test

bench: $(BENCH_TEST)
	$(BENCH_TEST) $(KRAIT)

$(BENCH_TEST): tests/bench_test.c $(BUILD_DIR)/src/native_compile.o $(BUILD_DIR)/src/native_util.o $(BUILD_DIR)/src/native_scaffold.o $(BUILD_DIR)/src/native_agent.o $(BUILD_DIR)/src/native_project.o $(BUILD_DIR)/src/native_preview.o $(BUILD_DIR)/src/native_live.o $(BUILD_DIR)/src/native_live_eval.o $(BUILD_DIR)/src/native_scene.o $(BUILD_DIR)/src/native_kanban.o $(BUILD_DIR)/src/native_krbhex.o $(BUILD_DIR)/src/native_ai.o $(KRYON_LIB) $(RAYLIB_A) $(KRYON_CURL_A) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -I$(KRAIT_GEN) -I$(KRYON_DIR)/include -o $@ tests/bench_test.c \
		$(BUILD_DIR)/src/native_compile.o $(BUILD_DIR)/src/native_util.o \
		$(BUILD_DIR)/src/native_scaffold.o $(BUILD_DIR)/src/native_agent.o \
		$(BUILD_DIR)/src/native_project.o $(BUILD_DIR)/src/native_preview.o \
		$(BUILD_DIR)/src/native_live.o $(BUILD_DIR)/src/native_live_eval.o \
		$(BUILD_DIR)/src/native_scene.o $(BUILD_DIR)/src/native_kanban.o \
		$(BUILD_DIR)/src/native_krbhex.o $(BUILD_DIR)/src/native_ai.o \
		-Wl,--whole-archive $(KRYON_LIB) -Wl,--no-whole-archive \
		$(RAYLIB_A) $(KRYON_BOX2D_A) $(KRYON_LIBOQS_A) \
		$(KRYON_CURL_LDLIBS) $(KRYON_MARKDOWN_LDLIBS) $(RAY_LDLIBS) \
		$(SYSTEM_THEME_LDLIBS) $(CURL_CODEC_LDLIBS) \
			-lbrotlidec -lbrotlicommon -lzstd -lz -lpthread -lm

agent-test: $(AGENT_TEST)
	$(AGENT_TEST)

$(AGENT_TEST): tests/agent_test.c $(BUILD_DIR)/src/native_agent.o $(BUILD_DIR)/src/native_md.o $(BUILD_DIR)/src/native_compile.o $(BUILD_DIR)/src/native_ai.o $(BUILD_DIR)/src/native_util.o $(BUILD_DIR)/src/native_scaffold.o $(BUILD_DIR)/src/native_project.o $(BUILD_DIR)/src/native_preview.o $(BUILD_DIR)/src/native_live.o $(BUILD_DIR)/src/native_live_eval.o $(BUILD_DIR)/src/native_scene.o $(BUILD_DIR)/src/native_kanban.o $(BUILD_DIR)/src/native_krbhex.o $(KRYON_LIB) $(RAYLIB_A) $(KRYON_CURL_A) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -I$(KRAIT_GEN) -I$(KRYON_DIR)/include -o $@ tests/agent_test.c \
		$(BUILD_DIR)/src/native_agent.o $(BUILD_DIR)/src/native_md.o $(BUILD_DIR)/src/native_compile.o \
		$(BUILD_DIR)/src/native_ai.o \
		$(BUILD_DIR)/src/native_util.o $(BUILD_DIR)/src/native_scaffold.o \
		$(BUILD_DIR)/src/native_project.o $(BUILD_DIR)/src/native_preview.o \
		$(BUILD_DIR)/src/native_live.o $(BUILD_DIR)/src/native_live_eval.o \
		$(BUILD_DIR)/src/native_scene.o $(BUILD_DIR)/src/native_kanban.o \
		$(BUILD_DIR)/src/native_krbhex.o \
		-Wl,--whole-archive $(KRYON_LIB) -Wl,--no-whole-archive \
		$(RAYLIB_A) $(KRYON_BOX2D_A) $(KRYON_LIBOQS_A) \
		$(KRYON_CURL_LDLIBS) $(KRYON_MARKDOWN_LDLIBS) $(RAY_LDLIBS) \
		$(SYSTEM_THEME_LDLIBS) $(CURL_CODEC_LDLIBS) \
			-lbrotlidec -lbrotlicommon -lzstd -lz -lpthread -lm

engine-test: $(ENGINE_TEST)
	$(ENGINE_TEST)

$(ENGINE_TEST): tests/engine_test.c $(BUILD_DIR)/src/native_engine.o $(BUILD_DIR)/src/native_script.o $(BUILD_DIR)/src/native_util.o $(BUILD_DIR)/src/native_scaffold.o $(BUILD_DIR)/src/native_level.o $(KRYON_LIB) $(RAYLIB_A) $(KRYON_BOX2D_A) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -I$(KRAIT_GEN) -I$(KRYON_DIR)/include -o $@ tests/engine_test.c \
		$(BUILD_DIR)/src/native_engine.o $(BUILD_DIR)/src/native_script.o \
		$(BUILD_DIR)/src/native_util.o \
		$(BUILD_DIR)/src/native_scaffold.o \
		\
		$(BUILD_DIR)/src/native_level.o \
		-Wl,--whole-archive $(KRYON_LIB) -Wl,--no-whole-archive \
		$(RAYLIB_A) $(KRYON_BOX2D_A) $(KRYON_LIBOQS_A) \
		$(KRYON_CURL_LDLIBS) $(KRYON_MARKDOWN_LDLIBS) $(RAY_LDLIBS) \
		$(SYSTEM_THEME_LDLIBS) $(CURL_CODEC_LDLIBS) \
			-lbrotlidec -lbrotlicommon -lzstd -lz -lpthread -lm

level: $(LEVEL)

$(LEVEL): src/level_main.c $(BUILD_DIR)/src/native_level.o $(KRYON_LIB) $(RAYLIB_A) | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -I$(KRAIT_GEN) -I$(KRYON_DIR)/include -o $@ src/level_main.c \
		$(BUILD_DIR)/src/native_level.o \
		-Wl,--whole-archive $(KRYON_LIB) -Wl,--no-whole-archive \
		$(RAYLIB_A) $(KRYON_BOX2D_A) $(KRYON_LIBOQS_A) \
		$(KRYON_CURL_LDLIBS) $(KRYON_MARKDOWN_LDLIBS) $(RAY_LDLIBS) \
		$(SYSTEM_THEME_LDLIBS) $(CURL_CODEC_LDLIBS) \
		-lbrotlidec -lbrotlicommon -lzstd -lz -lpthread -lm -ldl

smoke: test
	@kryon_dir=$$(cd "$(KRYON_DIR)" && pwd); \
	KRYON_DIR="$$kryon_dir" $(KRAIT) --smoke-screens samples/hello.kry /tmp/krait-smoke.png
	@kryon_dir=$$(cd "$(KRYON_DIR)" && pwd); \
	KRYON_DIR="$$kryon_dir" $(KRAIT) --smoke-live samples hello.kry
	@kryon_dir=$$(cd "$(KRYON_DIR)" && pwd); \
	KRYON_DIR="$$kryon_dir" $(KRAIT) --smoke-live samples live_widgets.kry
	@kryon_dir=$$(cd "$(KRYON_DIR)" && pwd); \
	KRYON_DIR="$$kryon_dir" $(KRAIT) --smoke-engine

install: $(KRAIT)
	mkdir -p $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 $(KRAIT) $(DESTDIR)$(BINDIR)/krait

# Stages the AppDir locally; bundles libraries and emits build/dist/*.AppImage
# when linuxdeploy + linuxdeploy-plugin-appimage are on PATH (CI does this).
appimage: $(KRAIT)
	sh packaging/appimage/build-appimage.sh

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/krait

boundary-check:
	sh tests/check-boundary.sh "$(KRYON_DIR)"

docs-site:
	rm -rf $(SITE_BUILD_DIR)
	mkdir -p $(SITE_BUILD_DIR)
	cp -R $(SITE_DIR)/. $(SITE_BUILD_DIR)/
	cp -R $(SITE_DIR)/cursors $(SITE_BUILD_DIR)/

clean:
	rm -rf $(BUILD_DIR)
