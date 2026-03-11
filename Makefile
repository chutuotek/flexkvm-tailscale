#
# Tailscale ARMv7 Cross-Compilation Makefile
# Target Platform: RV1106 (arm-rockchip830-linux-uclibcgnueabihf)
# Tag: v1.90.9
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
TAILSCALE_TAG := v1.90.9
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
	@cd $(TAILSCALE_SRC) && git checkout $(TAILSCALE_TAG) 2>/dev/null || true

build: clone
	@echo "Building tailscale..."
	@mkdir -p $(PKG_BIN)

	@( \
		export CGO_ENABLED=1 && \
		export GOOS=linux && \
		export GOARCH=arm && \
		export GOARM=7 && \
		export CC=$(CROSS_CC) && \
		export CXX=$(CROSS_CXX) && \
		export CGO_LDFLAGS="-static" && \
		cd $(TAILSCALE_SRC) && \
		TAGS=$$(./tool/go run ./cmd/featuretags --remove=bird,lazywg,tap,resolved,aws,kube,synology,appconnectors,dbus,networkmanager,syspolicy,desktop_sessions,systray,captiveportal,sdnotify,wakeonlan,clientupdate,ssh,tpm,linkspeed,webclient,drive,taildrop) && \
		echo "Building with tags: $$TAGS" && \
		go build -ldflags="-s -w -buildid=" -trimpath -gcflags='-l' -asmflags='-trimpath' -tags "$$TAGS" -o $(CURDIR)/$(PKG_BIN)/tailscale ./cmd/tailscale && \
		go build -ldflags="-s -w -buildid=" -trimpath -gcflags='-l' -asmflags='-trimpath' -tags "$$TAGS" -o $(CURDIR)/$(PKG_BIN)/tailscaled ./cmd/tailscaled \
	)

install: build
	@echo "Binaries ready in $(OUTPUT_DIR)/"

# Compress with upx
	@echo ""
	@echo "Compressing with upx..."
	@which upx >/dev/null 2>&1 && upx --best $(OUTPUT_DIR)/tailscale $(OUTPUT_DIR)/tailscaled || echo "  upx not found, skipping compression"

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
