# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# Build Plumlang.
#
#   make                  release build -> ./plum
#   make debug            debug build (-O0 -g)
#   make sanitize         debug build under AddressSanitizer + UBSan
#   make test             run the tests against the selected build
#   make sanitize test    the tests, under the sanitizers
#   make format           format the C sources with clang-format
#   make format-check     fail if anything is not formatted
#   make clean
#
# Each mode builds into its own directory under build/, and ./plum is a copy
# of the last binary built.

CC      ?= cc
STD     := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wvla -Wformat=2 -Wundef
INCLUDE := -Iinclude

COMMIT  := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
DEFS    := -DPLUM_COMMIT=\"$(COMMIT)\"

ifneq ($(filter sanitize,$(MAKECMDGOALS)),)
  MODE := sanitize
else ifneq ($(filter debug,$(MAKECMDGOALS)),)
  MODE := debug
else
  MODE := release
endif

ifeq ($(MODE),release)
  OPT := -O2 -DNDEBUG
else ifeq ($(MODE),debug)
  OPT := -O0 -g3
else
  OPT := -O1 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined \
         -fno-sanitize-recover=all
  LDSAN := -fsanitize=address,undefined
endif

CFLAGS  += $(STD) $(WARN) $(OPT) $(INCLUDE) $(DEFS) -MMD -MP
LDFLAGS += $(LDSAN)

BUILD   := build/$(MODE)
SRCS    := $(shell find src -name '*.c' | sort)
OBJS    := $(SRCS:%.c=$(BUILD)/%.o)
BIN     := $(BUILD)/plum
PLUM    := ./plum

.PHONY: all release debug sanitize test format format-check clean

# Always refresh ./plum, so it is the binary of the mode asked for even when
# another mode's copy is newer.
all release debug sanitize: $(BIN)
	@cp $(BIN) $(PLUM)
	@echo "built $(PLUM) ($(MODE))"

$(BIN): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

test: all
	@sh tests/smoke.sh $(PLUM)

C_FILES := $(shell find src include -name '*.c' -o -name '*.h' | sort)

format:
	clang-format -i $(C_FILES)

format-check:
	clang-format --dry-run --Werror $(C_FILES)

clean:
	rm -rf build $(PLUM)

-include $(OBJS:.o=.d)
