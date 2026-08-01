CC ?= cc
BUILD_DIR ?= build
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
INSTALL ?= install
KRYON_DIR ?= ../kryon
KRYON_BUILD_DIR ?= $(KRYON_DIR)/build
KRYON_PLATFORM ?= $(shell uname -s 2>/dev/null | tr '[:upper:]' '[:lower:]')

KITE = $(BUILD_DIR)/bin/kite
KRYON_IDE = $(BUILD_DIR)/bin/kryon-ide
KITE_GEN = $(BUILD_DIR)/gen
KITE_SRCS := $(wildcard ide/*.kry)
KITE_OBJS := $(patsubst ide/%.kry,$(KITE_GEN)/ide/%.o,$(KITE_SRCS))

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

.PHONY: all kite kryon-ide run test smoke clean install uninstall kryon-deps boundary-check

all: kite

kite: $(KITE)

kryon-ide: $(KRYON_IDE)

kryon-deps:
	$(MAKE) -C $(KRYON_DIR) all

$(KITE_GEN)/.transpiled: $(KITE_SRCS) | $(KC)
	@mkdir -p $(KITE_GEN)
	$(KC) --root . -o $(KITE_GEN) $(KITE_SRCS)
	@touch $@

$(KITE_GEN)/ide/%.o: $(KITE_GEN)/.transpiled
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -I$(KITE_GEN) -c $(KITE_GEN)/ide/$*.c -o $@

$(KITE): $(KITE_OBJS) $(KITE_GEN)/.transpiled $(KRYON_LIB) $(RAYLIB_A) $(KRYON_LIBOQS_A) $(KRYON_CURL_A) $(KRYON_CMARK_GFM_A) $(KRYON_CMARK_GFM_EXTENSIONS_A) | $(BUILD_DIR)/bin
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(KITE_GEN) $(RAY_CFLAGS) $(SYSTEM_THEME_CFLAGS) -o $@ \
		$(KITE_OBJS) \
		-Wl,--whole-archive $(KRYON_LIB) -Wl,--no-whole-archive \
		$(RAYLIB_A) $(RAY_LDLIBS) $(KRYON_LIBOQS_A) $(KRYON_CURL_LDLIBS) \
		$(KRYON_MARKDOWN_LDLIBS) \
		-Wl,-export-dynamic $(KRYON_PLATFORM_LDLIBS) \
		$(SYSTEM_THEME_LDLIBS) $(CURL_CODEC_LDLIBS) -lz -lpthread -lm

$(KRYON_IDE): $(KITE) | $(BUILD_DIR)/bin
	ln -sf kite $@

$(KC) $(KRYON_LIB) $(RAYLIB_A) $(KRYON_LIBOQS_A) $(KRYON_CURL_A) $(KRYON_CMARK_GFM_A) $(KRYON_CMARK_GFM_EXTENSIONS_A):
	$(MAKE) -C $(KRYON_DIR) all

$(BUILD_DIR)/bin:
	mkdir -p $@

run: kite
	@kryon_dir=$$(cd "$(KRYON_DIR)" && pwd); \
	KRYON_DIR="$$kryon_dir" $(KITE) $(ARGS)

test: kite boundary-check

smoke: test
	@printf '%s\n' 'KITE smoke build passed. Runtime preview smoke will be added with the KITE CLI smoke runner.'

install: $(KITE)
	mkdir -p $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 $(KITE) $(DESTDIR)$(BINDIR)/kite
	ln -sf kite $(DESTDIR)$(BINDIR)/kryon-ide

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kite $(DESTDIR)$(BINDIR)/kryon-ide

boundary-check:
	sh tests/check-boundary.sh "$(KRYON_DIR)"

clean:
	rm -rf $(BUILD_DIR)
