# SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
#
# Build Plumlang.
#
#   make                  release build -> ./plum
#   make debug            debug build (-O0 -g)
#   make sanitize         debug build under AddressSanitizer + UBSan
#   make test             unit, smoke, harness, golden and formatter tests on the selected build
#   make sanitize test    the tests, under the sanitizers
#   make golden-update    rewrite golden expectations from actual output (and the REPL's)
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
INCLUDE := -Iinclude -Isrc

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

# Mbed TLS, vendored under vendor/mbedtls (see its README.md). It is built
# with its own, relaxed warnings; our code sees its headers as system
# headers, so our strict warnings stay on for what we write.
MBEDTLS   := vendor/mbedtls
TFPSA     := $(MBEDTLS)/tf-psa-crypto
TLS_DEFS  := -DMBEDTLS_USER_CONFIG_FILE=\"mbedtls_user_config.h\" \
             -DTF_PSA_CRYPTO_USER_CONFIG_FILE=\"crypto_user_config.h\"
TLS_INCLUDE := -isystem $(MBEDTLS)/include -isystem $(TFPSA)/include \
               -isystem $(TFPSA)/drivers/builtin/include -Isrc/stdlib/tls
VENDOR_INCLUDE := -I$(MBEDTLS)/include -I$(MBEDTLS)/library -I$(TFPSA)/include \
                  -I$(TFPSA)/drivers/builtin/include -I$(TFPSA)/core \
                  -I$(TFPSA)/drivers/builtin/src -I$(TFPSA)/dispatch -I$(TFPSA)/extras \
                  -I$(TFPSA)/platform -I$(TFPSA)/utilities -Isrc/stdlib/tls
# Each function in a section of its own, so the linker drops what plum
# never calls (about 120 KB of the library).
VENDOR_CFLAGS := $(STD) $(OPT) $(VENDOR_INCLUDE) $(TLS_DEFS) -ffunction-sections \
                 -fdata-sections -MMD -MP
ifeq ($(shell uname -s 2>/dev/null),Darwin)
  LDGC := -Wl,-dead_strip
else
  LDGC := -Wl,--gc-sections
endif

CFLAGS  += $(STD) $(WARN) $(OPT) $(INCLUDE) $(TLS_INCLUDE) $(TLS_DEFS) $(DEFS) -MMD -MP
LDFLAGS += $(LDSAN) $(LDGC)
LDLIBS  += -lm
ifeq ($(OS),Windows_NT)
# Winsock for the net module; BCrypt (random numbers) and Crypt32 (the
# system's trusted certificates) for TLS
LDLIBS  += -lws2_32 -lbcrypt -lcrypt32
endif

BUILD   := build/$(MODE)
# Everything but the command-line front end is the library the unit tests
# link against.
LIB_SRCS  := $(shell find src -name '*.c' ! -path 'src/cli/*' | sort)
CLI_SRCS  := $(shell find src/cli -name '*.c' | sort)
UNIT_SRCS := $(shell find tests/unit -name '*.c' | sort)
VENDOR_SRCS := $(sort $(wildcard $(MBEDTLS)/library/*.c $(TFPSA)/core/*.c \
                 $(TFPSA)/drivers/builtin/src/*.c $(TFPSA)/dispatch/*.c $(TFPSA)/extras/*.c \
                 $(TFPSA)/platform/*.c $(TFPSA)/utilities/*.c))
LIB_OBJS  := $(LIB_SRCS:%.c=$(BUILD)/%.o) $(VENDOR_SRCS:%.c=$(BUILD)/%.o)
CLI_OBJS  := $(CLI_SRCS:%.c=$(BUILD)/%.o)
UNIT_OBJS := $(UNIT_SRCS:%.c=$(BUILD)/%.o)
OBJS      := $(LIB_OBJS) $(CLI_OBJS) $(UNIT_OBJS)
BIN       := $(BUILD)/plum
UNIT_BIN  := $(BUILD)/unit-tests
PLUM      := ./plum

.PHONY: all release debug sanitize test golden-update bytecode-doc stdlib-doc stdlib-embed format format-check clean

# Always refresh ./plum, so it is the binary of the mode asked for even when
# another mode's copy is newer.
all release debug sanitize: $(BIN)
	@cp $(BIN) $(PLUM)
	@echo "built $(PLUM) ($(MODE))"

$(BIN): $(LIB_OBJS) $(CLI_OBJS)
	$(CC) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(UNIT_BIN): $(LIB_OBJS) $(UNIT_OBJS)
	$(CC) $^ $(LDFLAGS) $(LDLIBS) -o $@

# Third-party code first: make 3.81 (macOS) takes the first rule that matches.
$(BUILD)/vendor/%.o: vendor/%.c
	@mkdir -p $(dir $@)
	@echo "  CC (vendor) $<"
	@$(CC) $(VENDOR_CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

test: all $(UNIT_BIN)
	@$(UNIT_BIN)
	@sh tests/smoke.sh $(PLUM)
	@sh tests/harness/self-test.sh
	@sh tests/errors-documented.sh
	@sh tests/bytecode-documented.sh
	@sh tools/stdlib-docs.sh $(PLUM) --check
	@sh tests/stdlib-embedded.sh
	@sh tests/golden.sh $(PLUM)
	@sh tests/golden.sh $(PLUM) examples
	@sh tests/fmt-check.sh $(PLUM)
	@sh tests/repl.sh $(PLUM)

# Regenerate the instruction table in docs/bytecode.md from opcodes.h.
bytecode-doc:
	@mkdir -p build
	@sh tools/bytecode-table.sh > build/opcodes.md
	@awk 'BEGIN { while ((getline line < "build/opcodes.md") > 0) table = table line "\n" } \
	      /<!-- opcodes:end -->/ { printf "%s", table; skip = 0 } \
	      !skip { print } \
	      /<!-- opcodes:begin -->/ { skip = 1 }' docs/bytecode.md > build/bytecode.md
	@mv build/bytecode.md docs/bytecode.md
	@echo "docs/bytecode.md updated"

# Build the modules written in Plumlang into the binary.
stdlib-embed:
	@sh tools/embed-stdlib.sh > src/stdlib/embedded.c
	@echo "src/stdlib/embedded.c updated"

# Regenerate the reference tables in docs/stdlib from the modules themselves.
stdlib-doc: all
	@sh tools/stdlib-docs.sh $(PLUM)

# Rewrite the golden expectations from actual output (review the diff!).
golden-update: all
	@UPDATE=1 sh tests/golden.sh $(PLUM)
	@UPDATE=1 sh tests/golden.sh $(PLUM) examples
	@UPDATE=1 sh tests/repl.sh $(PLUM)

# Ours only: vendor/ keeps its upstream formatting.
C_FILES := $(shell find src include tests/unit -name '*.c' -o -name '*.h' | sort)
C_FILES := $(filter-out src/stdlib/embedded.c,$(C_FILES)) # generated

format:
	clang-format -i $(C_FILES)

format-check:
	clang-format --dry-run --Werror $(C_FILES)

clean:
	rm -rf build $(PLUM)

-include $(OBJS:.o=.d)
