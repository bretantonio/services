# ============================
#  GLOBAL SETTINGS
# ============================

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?=

PREFIX  ?= /usr

TARGETS = wmt_manager services sepolicy

# ============================
#  SELinux Policy Settings
# ============================

POLICY_SRC := minimal.conf
POLICY_BIN := sepolicy
POLICY_VER := 30

CHECKPOLICY ?= checkpolicy
SEINFO      ?= seinfo
SESEARCH    ?= sesearch

# ============================
#  DEFAULT TARGET
# ============================

all: $(TARGETS)
	@echo "Build complete."

# ============================
#  BUILD C BINARIES
# ============================

wmt_manager: main.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	strip --strip-unneeded $@

services: services.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	strip --strip-unneeded $@

# ============================
#  BUILD SELinux POLICY
# ============================

sepolicy: $(POLICY_SRC)
	$(CHECKPOLICY) -c $(POLICY_VER) -o $(POLICY_BIN) $<
	@echo "Compiled $< → $(POLICY_BIN)"

# ============================
#  ANALYSIS TARGETS
# ============================

check: sepolicy stats rules

stats: sepolicy
	@echo ""
	@echo "=== Policy statistics ==="
	$(SEINFO) $(POLICY_BIN) -c -r -t -u --common
	@echo ""

rules: sepolicy
	@echo ""
	@echo "=== Allow rules in compiled policy ==="
	$(SESEARCH) --allow $(POLICY_BIN)
	@echo ""
	@echo "--- Rule count ---"
	@count=$$($(SESEARCH) --allow $(POLICY_BIN) | grep -c '^allow' || true); \
	  echo "  $$count allow rule(s) found"; \
	  if [ "$$count" -eq 0 ]; then \
	    echo "ERROR: no allow rules compiled in — policy may be empty"; \
	    exit 1; \
	  fi

# ============================
#  INSTALL
# ============================

install: wmt_manager services
	install -m 0755 wmt_manager /sbin/wmt_manager
	install -m 0755 services $(PREFIX)/sbin/services

# ============================
#  CLEAN
# ============================

clean:
	rm -f wmt_manager services sepolicy
