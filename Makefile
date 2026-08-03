CC ?= cc
BUILD_DIR ?= build
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
INSTALL ?= install
KRYON_DIR ?= vendor/kryon
DEV_KRYON_DIR ?= ../kryon
KRYON_BUILD_DIR ?= $(KRYON_DIR)/build
KRYON_PLATFORM ?= $(shell uname -s 2>/dev/null | tr '[:upper:]' '[:lower:]')

KRAIT = $(BUILD_DIR)/bin/krait
KRAIT_GEN = $(BUILD_DIR)/gen
KRAIT_SRCS := $(wildcard ide/*.kry)
KRAIT_OBJS := $(patsubst ide/%.kry,$(KRAIT_GEN)/ide/%.o,$(KRAIT_SRCS))
KRAIT_NATIVE_SRCS := src/main.c src/native.c
KRAIT_NATIVE_OBJS := $(patsubst src/%.c,$(BUILD_DIR)/src/%.o,$(KRAIT_NATIVE_SRCS))

KC = $(KRYON_BUILD_DIR)/bin/kc
KRYON_LIB = $(KRYON_DIR)/libkryon.a
RAYLIB_A = $(KRYON_BUILD_DIR)/raylib/libraylib.a
KRYON_LIBOQS_A = $(KRYON_BUILD_DIR)/vendor/liboqs/lib/liboqs.a
KRYON_CURL_A = $(KRYON_BUILD_DIR)/vendor/curl/lib/libcurl.a
KRYON_CMARK_GFM_A = $(KRYON_BUILD_DIR)/vendor/cmark-gfm/src/libcmark-gfm.a
KRYON_CMARK_GFM_EXTENSIONS_A = $(KRYON_BUILD_DIR)/vendor/cmark-gfm/extensions/libcmark-gfm-extensions.a

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
CPPFLAGS += -I$(KRYON_DIR)/include -I$(KRYON_DIR)/src/ui -I$(KRYON_DIR)/vendor/clay \
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

.PHONY: all krait run dev test smoke clean install uninstall kryon-deps boundary-check

all: krait

krait: $(KRAIT)

kryon-deps:
	$(MAKE) -C $(KRYON_DIR) all

$(KRAIT_GEN)/.transpiled: $(KRAIT_SRCS) | $(KC)
	@mkdir -p $(KRAIT_GEN)
	$(KC) --root . -o $(KRAIT_GEN) $(KRAIT_SRCS)
	@touch $@

$(KRAIT_GEN)/ide/app.o: $(KRAIT_GEN)/.transpiled
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -Dmain=krait_generated_main -I$(KRAIT_GEN) -c $(KRAIT_GEN)/ide/app.c -o $@

$(KRAIT_GEN)/ide/%.o: $(KRAIT_GEN)/.transpiled
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -I$(KRAIT_GEN) -c $(KRAIT_GEN)/ide/$*.c -o $@

$(BUILD_DIR)/src/%.o: src/%.c $(KRAIT_GEN)/.transpiled
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -I$(KRAIT_GEN) -c $< -o $@

$(KRAIT): kryon-deps $(KRAIT_OBJS) $(KRAIT_NATIVE_OBJS) $(KRAIT_GEN)/.transpiled $(KRYON_LIB) $(RAYLIB_A) $(KRYON_LIBOQS_A) $(KRYON_CURL_A) $(KRYON_CMARK_GFM_A) $(KRYON_CMARK_GFM_EXTENSIONS_A) | $(BUILD_DIR)/bin
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(KRAIT_GEN) $(RAY_CFLAGS) $(SYSTEM_THEME_CFLAGS) -o $@ \
		$(KRAIT_OBJS) $(KRAIT_NATIVE_OBJS) \
		-Wl,--whole-archive $(KRYON_LIB) -Wl,--no-whole-archive \
		$(RAYLIB_A) $(RAY_LDLIBS) $(KRYON_LIBOQS_A) $(KRYON_CURL_LDLIBS) \
		$(KRYON_MARKDOWN_LDLIBS) \
		-Wl,-export-dynamic $(KRYON_PLATFORM_LDLIBS) \
		$(SYSTEM_THEME_LDLIBS) $(CURL_CODEC_LDLIBS) -lz -lpthread -lm

$(KC) $(KRYON_LIB) $(KRYON_LIBOQS_A) $(KRYON_CURL_A) $(KRYON_CMARK_GFM_A) $(KRYON_CMARK_GFM_EXTENSIONS_A):
	$(MAKE) -C $(KRYON_DIR) all

$(RAYLIB_A):
	$(MAKE) -C $(KRYON_DIR) $(patsubst $(KRYON_DIR)/%,%,$@)

$(BUILD_DIR)/bin:
	mkdir -p $@

run: krait
	@kryon_dir=$$(cd "$(KRYON_DIR)" && pwd); \
	KRYON_DIR="$$kryon_dir" $(KRAIT) $(ARGS)

dev: KRYON_DIR := $(DEV_KRYON_DIR)
dev: krait
	@kryon_dir=$$(cd "$(KRYON_DIR)" && pwd); \
	KRYON_DIR="$$kryon_dir" $(KRAIT) --kryon-dir "$$kryon_dir" $(ARGS)

test: krait boundary-check

smoke: test
	@kryon_dir=$$(cd "$(KRYON_DIR)" && pwd); \
	KRYON_DIR="$$kryon_dir" $(KRAIT) --smoke-screens samples/hello.kry /tmp/krait-smoke.png

install: $(KRAIT)
	mkdir -p $(DESTDIR)$(BINDIR)
	mkdir -p $(DESTDIR)$(PREFIX)/share/krait/assets/fonts
	$(INSTALL) -m 755 $(KRAIT) $(DESTDIR)$(BINDIR)/krait
	$(INSTALL) -m 644 assets/fonts/DepartureMono-Regular.otf $(DESTDIR)$(PREFIX)/share/krait/assets/fonts/DepartureMono-Regular.otf

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/krait
	rm -f $(DESTDIR)$(PREFIX)/share/krait/assets/fonts/DepartureMono-Regular.otf

boundary-check:
	sh tests/check-boundary.sh "$(KRYON_DIR)"

clean:
	rm -rf $(BUILD_DIR)
