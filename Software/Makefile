# Convenience wrapper around CMake + the code-style tooling.
#
#   make build     configure (if needed) and compile the firmware -- Debug by
#                  default, into build/debug/ (BUILD_TYPE=Release -> build/release/)
#   make release   shorthand for `make build BUILD_TYPE=Release`, into build/release/
#   make rebuild   wipe the current BUILD_TYPE's build dir and build from scratch
#   make test      configure + build the host unit tests and run ctest
#   make check     verify formatting  (clang-format, non-mutating)
#   make all       check, then test, then build
#   make flash-full MODULE=<name>   flash bootloader+app (bench factory programming;
#                  Release by default -- add BUILD_TYPE=Debug for a debug unit)
#   make flash MODULE=<name>        flash just the app (no bootloader merge)
#   make clean     remove build/ (both build types) and build-test/
#   make format    reformat all sources in place (clang-format -i)
#   make tidy      run clang-tidy over our sources (best-effort)
#
# Overridable: BUILD_DIR, TEST_BUILD_DIR, BUILD_TYPE, MODULE, JOBS, CMAKE,
# CLANG_FORMAT, CLANG_TIDY

BUILD_TYPE     ?= Debug
# build/debug or build/release -- keeps the two configurations side by side
# instead of one clobbering the other. `release`/`flash`/`flash-full` below
# get a different BUILD_TYPE default by recursing into a fresh $(MAKE) with
# it set on that invocation's own command line, rather than by overriding
# this variable in-process -- a target-specific override here would arrive
# too late to change $(BUILD_DIR)/CMakeCache.txt, which is fixed as soon as
# `configure`'s rule is first parsed, before any target-specific value applies.
BUILD_DIR      ?= build/$(shell echo $(BUILD_TYPE) | tr '[:upper:]' '[:lower:]')
TEST_BUILD_DIR ?= build-test
JOBS           ?= $(shell nproc)
CMAKE          ?= cmake
CLANG_FORMAT   ?= clang-format
CLANG_TIDY     ?= clang-tidy

# Our own source only - never the vendored STM32 HAL/CMSIS tree or the
# ST-templated hal_conf header. Covers the host test tree (test/, Lib/*/test,
# Modules/*/test) too; the 'stm32*' filter also drops test/fake's HAL header
# double.
SRC_DIRS  := $(wildcard Lib/HAL Lib/Board Lib/NodeLib Lib/Tools Modules test)
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

.PHONY: help build release configure rebuild test all clean check format tidy flash flash-full
.DEFAULT_GOAL := help

help:
	@awk '/^#/{sub(/^# ?/, ""); print; next} {exit}' $(firstword $(MAKEFILE_LIST))

$(CACHE):
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

configure: $(CACHE)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j $(JOBS)

# `make release` == `make build BUILD_TYPE=Release`, just without having to
# type it. A target-specific `BUILD_TYPE := Release` here would NOT work --
# BUILD_DIR's prerequisite path is fixed to Debug's the first time `configure`
# is parsed, before any target-specific override takes effect, regardless of
# which target later pulls it in. Recursing into a fresh $(MAKE) sidesteps
# that: BUILD_TYPE arrives as a real command-line variable from the start of
# that invocation, the same as if you'd typed `make build BUILD_TYPE=Release`
# yourself, so BUILD_DIR resolves to build/release throughout.
release:
	$(MAKE) build BUILD_TYPE=Release

rebuild: clean build

# Factory bench programming (tools/README.md): defaults to the Release build
# since that's what ships, not what's convenient to step through in a
# debugger. Pass BUILD_TYPE=Debug to flash a debug unit instead.
#
# BUILD_TYPE := Release is a target-specific default (overridable on the
# command line, same as any other target-specific variable -- see `release`
# above for why plain `?=` at the top of the file can't do this by itself);
# recursing into $(MAKE) is what makes BUILD_DIR resolve correctly once that
# value crosses into the shared `build`/`configure` chain.
#   make flash-full MODULE=temperatureNode                  # Release (default)
#   make flash-full MODULE=temperatureNode BUILD_TYPE=Debug  # Debug
flash-full: BUILD_TYPE := Release
flash-full:
	@test -n "$(MODULE)" || { echo "usage: make flash-full MODULE=<name> [BUILD_TYPE=Debug]"; exit 1; }
	$(MAKE) build BUILD_TYPE=$(BUILD_TYPE)
	$(CMAKE) --build build/$(shell echo $(BUILD_TYPE) | tr '[:upper:]' '[:lower:]') --target flash-$(MODULE)-full

flash: BUILD_TYPE := Release
flash:
	@test -n "$(MODULE)" || { echo "usage: make flash MODULE=<name> [BUILD_TYPE=Debug]"; exit 1; }
	$(MAKE) build BUILD_TYPE=$(BUILD_TYPE)
	$(CMAKE) --build build/$(shell echo $(BUILD_TYPE) | tr '[:upper:]' '[:lower:]') --target flash-$(MODULE)

# Host-native build (its own compiler, not the ARM toolchain) -- see test/.
test:
	$(CMAKE) -S test -B $(TEST_BUILD_DIR)
	$(CMAKE) --build $(TEST_BUILD_DIR) -j $(JOBS)
	$(CMAKE) -E chdir $(TEST_BUILD_DIR) ctest --output-on-failure

all: check test build

clean:
	rm -rf build $(TEST_BUILD_DIR)

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
