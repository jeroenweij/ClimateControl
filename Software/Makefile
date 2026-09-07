# Convenience wrapper around CMake + the code-style tooling.
#
#   make build     configure (if needed) and compile into build/
#   make rebuild   wipe build/ and build from scratch
#   make clean     remove build/
#   make check     verify formatting  (clang-format, non-mutating)
#   make format    reformat all sources in place (clang-format -i)
#   make tidy      run clang-tidy over our sources (best-effort)
#
# Overridable: BUILD_DIR, BUILD_TYPE, JOBS, CMAKE, CLANG_FORMAT, CLANG_TIDY

BUILD_DIR    ?= build
BUILD_TYPE   ?= Debug
JOBS         ?= $(shell nproc)
CMAKE        ?= cmake
CLANG_FORMAT ?= clang-format
CLANG_TIDY   ?= clang-tidy

# Our own source only - never the vendored STM32 HAL/CMSIS tree or the
# ST-templated hal_conf header.
SRC_DIRS  := $(wildcard Lib/HAL Lib/Board Lib/NodeLib Lib/Tools Modules)
SOURCES   := $(shell find $(SRC_DIRS) -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.cpp' \) \
                     -not -path '*/Vendor/*' -not -name 'stm32*')

CACHE := $(BUILD_DIR)/CMakeCache.txt

# clang-tidy runs host clang, so hand it arm-none-eabi's own include search
# path (newlib + libstdc++) or every standard header comes up missing.
CROSS       ?= arm-none-eabi-g++
TIDY_ARGS   := $(shell $(CROSS) -E -x c++ -v /dev/null 2>&1 \
                 | sed -n '/search starts here/,/End of search/p' \
                 | sed -n 's|^ /|--extra-arg=-isystem/|p') \
               --extra-arg=--target=arm-none-eabi

.PHONY: help build configure rebuild clean check format tidy
.DEFAULT_GOAL := help

help:
	@awk '/^#/{sub(/^# ?/, ""); print; next} {exit}' $(firstword $(MAKEFILE_LIST))

$(CACHE):
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

configure: $(CACHE)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j $(JOBS)

rebuild: clean build

clean:
	rm -rf $(BUILD_DIR)

check:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || { echo "clang-format not found - 'sudo apt install clang-format'"; exit 1; }
	@echo "clang-format: checking $(words $(SOURCES)) files"
	@$(CLANG_FORMAT) --dry-run --Werror $(SOURCES)
	@echo "format OK"

format:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || { echo "clang-format not found - 'sudo apt install clang-format'"; exit 1; }
	@$(CLANG_FORMAT) -i $(SOURCES)
	@echo "reformatted $(words $(SOURCES)) files"

tidy: configure
	@command -v $(CLANG_TIDY) >/dev/null 2>&1 || { echo "clang-tidy not found - 'sudo apt install clang-tidy'"; exit 1; }
	@$(CLANG_TIDY) --quiet -p $(BUILD_DIR) $(TIDY_ARGS) $(SOURCES)
