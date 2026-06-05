.DEFAULT_GOAL := help

CONFIG ?= .config
DEFCONFIG ?= config/defconfig
BUILD_DIR ?= build
CMAKE_BUILD_TYPE ?= Release

.PHONY: help defconfig cmake build clean

help:
	@echo "MCP server configuration targets:"
	@echo "  make defconfig   - write $(CONFIG) from $(DEFCONFIG)"
	@echo "  make cmake       - configure CMake with $(CONFIG)"
	@echo "  make build       - configure and build"
	@echo "  make clean       - clean the configured build directory"

defconfig:
	cp "$(DEFCONFIG)" "$(CONFIG)"
	@echo "wrote $(CONFIG)"

cmake:
	cmake -S . -B "$(BUILD_DIR)" -DMCP_KCONFIG_CONFIG="$(abspath $(CONFIG))" -DCMAKE_BUILD_TYPE="$(CMAKE_BUILD_TYPE)"

build: cmake
	cmake --build "$(BUILD_DIR)" --parallel

clean:
	cmake --build "$(BUILD_DIR)" --target clean
