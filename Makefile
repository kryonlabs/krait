GMAKE ?= gmake

GMAKE_ARGS =
.if defined(KRYON_DIR)
GMAKE_ARGS += KRYON_DIR='${KRYON_DIR}'
.endif
.if defined(ARGS)
GMAKE_ARGS += ARGS='${ARGS}'
.endif

.PHONY: all krait run dev test smoke clean install uninstall kryon-deps boundary-check

all krait run dev test smoke clean install uninstall kryon-deps boundary-check:
	@command -v $(GMAKE) >/dev/null 2>&1 || { \
		echo "Krait uses GNU make; install gmake or run build/<platform>-<arch>/bin/krait directly."; \
		exit 1; \
	}
	@$(GMAKE) -f GNUmakefile ${.TARGET} $(GMAKE_ARGS)
