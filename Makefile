#
# Tailscale ARMv7 Cross-Compilation Makefile
# Target Platform: RV1106 (arm-rockchip830-linux-uclibcgnueabihf)
# Tag: v1.100.0
#

SHELL:=/bin/bash
export LC_ALL=C

# Build output configuration
PKG_NAME := tailscale
PKG_BIN ?= out
OUTPUT_DIR := $(PKG_BIN)
CURRENT_DIR := $(shell pwd)

# Toolchain settings (use CROSS_COMPILE from environment or default)
CROSS_COMPILE ?= arm-rockchip830-linux-uclibcgnueabihf-
CROSS_CC := $(CROSS_COMPILE)gcc
CROSS_CXX := $(CROSS_COMPILE)g++

# Tailscale version
TAILSCALE_TAG := v1.100.0
TAILSCALE_SRC := $(CURDIR)/tailscale

# Number of parallel jobs
JOBS ?= $(shell nproc)

.PHONY: all clean clone info help

all: info clone build install
	@echo "Build $(PKG_NAME) done"

info:
	@echo "=========================================="
	@echo "Tailscale ARMv7 Cross-Compilation"
	@echo "=========================================="
	@echo "Version: $(TAILSCALE_TAG)"
	@echo "Toolchain: $(CROSS_CC)"
	@echo "Source: $(TAILSCALE_SRC)"
	@echo "Output Dir: $(OUTPUT_DIR)"
	@echo "=========================================="

clone:
	@echo "Initializing/Updating tailscale submodule $(TAILSCALE_TAG)..."
	@git submodule update --init --force tailscale
	@cd $(TAILSCALE_SRC) && git fetch --tags 2>/dev/null && git checkout $(TAILSCALE_TAG) 2>/dev/null || true

build: clone
	@echo "Building tailscale..."
	@mkdir -p $(PKG_BIN)

	@# Apply patches (re-apply when patch count changes)
	@EXPECTED=$$(ls $(CURDIR)/patch/*.patch 2>/dev/null | wc -l); \
	CURRENT=$$(cat $(TAILSCALE_SRC)/.patched 2>/dev/null || echo 0); \
	if [ "$$CURRENT" != "$$EXPECTED" ]; then \
		echo "Patching: $$CURRENT -> $$EXPECTED patches"; \
		git -C $(TAILSCALE_SRC) checkout -- . 2>/dev/null; \
		for p in $(CURDIR)/patch/*.patch; do \
			echo "Applying: $$(basename $$p)"; \
			git -C $(TAILSCALE_SRC) apply --ignore-whitespace "$$p" || exit 1; \
		done; \
		echo "$$EXPECTED" > $(TAILSCALE_SRC)/.patched; \
	fi

	@# Step 1: detect feature tags and version (runs on host arch, NOT cross-compiled)
	@echo "Detecting feature tags..."
	@VERSION="1.100.0-1"; \
	TAGS=$$(cd $(TAILSCALE_SRC) && GOTOOLCHAIN=local go run ./cmd/featuretags --remove=bird,tap,resolved,aws,kube,synology,appconnectors,dbus,networkmanager,syspolicy,desktop_sessions,systray,captiveportal,sdnotify,wakeonlan,clientupdate,ssh,tpm,linkspeed,webclient,drive,taildrop,routecheck,serve,tailnetlock,tundevstats,netlog,clientmetrics,usermetrics,runtimemetrics,capture,advertiseexitnode,useexitnode,advertiseroutes,acme,ace,posture,outboundproxy,conn25,c2n,cloud,doctor,identityfederation,linuxdnsfight,qrcodes,useproxy,webbrowser,debugeventbus,debugportmapper,relayserver); \
	echo "Version: $$VERSION"; \
	echo "Building with tags: $$TAGS"; \
	( \
		export CGO_ENABLED=1 && \
		export GOOS=linux && \
		export GOARCH=arm && \
		export GOARM=7 && \
		export CC=$(CROSS_CC) && \
		export CXX=$(CROSS_CXX) && \
		export CGO_LDFLAGS="-static" && \
		export GOTOOLCHAIN=local && \
		cd $(TAILSCALE_SRC) && \
		LDFLAGS="-s -w -buildid= -X tailscale.com/version.longStamp=$$VERSION -X tailscale.com/version.shortStamp=$$VERSION" && \
		go build -ldflags="$$LDFLAGS" -trimpath -gcflags='-l' -asmflags='-trimpath' -tags "$$TAGS" -o $(CURDIR)/$(PKG_BIN)/tailscale ./cmd/tailscale && \
		go build -ldflags="$$LDFLAGS" -trimpath -gcflags='-l' -asmflags='-trimpath' -tags "$$TAGS" -o $(CURDIR)/$(PKG_BIN)/tailscaled ./cmd/tailscaled \
	)

install: build
	@echo "Binaries ready in $(OUTPUT_DIR)/"

# Compress with upx
	@echo ""
	@echo "Compressing with upx..."
	@which upx >/dev/null 2>&1 && upx --best --lzma $(OUTPUT_DIR)/tailscale $(OUTPUT_DIR)/tailscaled || echo "  upx not found, skipping compression"

	@echo ""
	@echo "Installed files:"
	@ls -lh $(OUTPUT_DIR)/

clean:
	@if [ -d "$(TAILSCALE_SRC)" ]; then \
		cd $(TAILSCALE_SRC) && git checkout . && git clean -fd; \
	fi
	rm -rf $(PKG_BIN)
	@echo "Clean done"

help:
	@echo "Tailscale Build Targets:"
	@echo ""
	@echo "  make          - Build tailscale"
	@echo "  make clone    - Clone/update tailscale source"
	@echo "  make build    - Build only"
	@echo "  make install  - Install binaries"
	@echo "  make clean    - Clean build artifacts"
	@echo "  make help     - Show this help"
