# SPDX-License-Identifier: MIT
# Copyright (c) 2022 Chris Dragan

##############################################################################
# Targets and sources

targets += minify
minify_src_files += arith_decode.c
minify_src_files += arith_encode.c
minify_src_files += bit_emit.c
minify_src_files += bit_stream.c
minify_src_files += find_repeats.c
minify_src_files += load_file.c
minify_src_files += lza_compress.c
minify_src_files += lza_decompress.c
minify_src_files += minify.c

targets += arith_encoder
arith_encoder_src_files += arith_decode.c
arith_encoder_src_files += arith_encode_file.c
arith_encoder_src_files += arith_encode.c
arith_encoder_src_files += bit_emit.c
arith_encoder_src_files += bit_stream.c
arith_encoder_src_files += load_file.c

tests += test_repeats
test_repeats_src_files += test_repeats.c
test_repeats_src_files += find_repeats.c

tests += test_arith_encode
test_arith_encode_src_files += arith_decode.c
test_arith_encode_src_files += arith_encode.c
test_arith_encode_src_files += bit_emit.c
test_arith_encode_src_files += bit_stream.c
test_arith_encode_src_files += test_arith_encode.c

##############################################################################
# Determine target OS

UNAME = $(shell uname -s)
ARCH ?= $(shell uname -m)

ifneq (,$(filter CYGWIN% MINGW% MSYS%, $(UNAME)))
    # Note: Still use cl.exe on Windows
    UNAME = Windows
endif

##############################################################################
# Default build flags

# Debug vs release
debug ?= 0

##############################################################################
# Compiler flags

ifeq ($(UNAME), Windows)
    WFLAGS += -W3

    ifeq ($(debug), 0)
        CFLAGS  += -O2 -Oi -DNDEBUG -GL -MT
        LDFLAGS += -ltcg
    else
        CFLAGS  += -D_DEBUG -Zi -MTd
        LDFLAGS += -debug
    endif

    CFLAGS += -nologo
    CFLAGS += -GR-
    CFLAGS += -TP -EHa-
    CFLAGS += -FS

    LDFLAGS += -nologo
    LDFLAGS += user32.lib kernel32.lib ole32.lib

    CC   = cl.exe
    LINK = link.exe

    COMPILER_OUTPUT = -Fo:$1
    LINKER_OUTPUT   = -out:$1
else
    WFLAGS += -Wall -Wextra -Wno-unused-parameter -Wunused -Wno-missing-field-initializers
    WFLAGS += -Wshadow -Wformat=2 -Wconversion -Wdouble-promotion
    WFLAGS += -Werror

    CFLAGS += -fvisibility=hidden
    CFLAGS += -fPIC
    CFLAGS += -MD

    ifeq ($(debug), 0)
        CFLAGS += -DNDEBUG -O3
        CFLAGS += -fomit-frame-pointer
        CFLAGS += -fno-stack-check -fno-stack-protector
	# For C++:
	#CFLAGS += -fno-threadsafe-statics

        CFLAGS  += -ffunction-sections -fdata-sections
        LDFLAGS += -ffunction-sections -fdata-sections
    else
        CFLAGS  += -fsanitize=address
        LDFLAGS += -fsanitize=address

        CFLAGS += -O0 -g
    endif

    LINK = $(CC)

    COMPILER_OUTPUT = -o $1
    LINKER_OUTPUT   = -o $1
endif

ifeq ($(UNAME), Linux)
    ifeq ($(debug), 0)
        STRIP = strip

        LDFLAGS += -Wl,--gc-sections -Wl,--as-needed

        LTO_CFLAGS += -flto -fno-fat-lto-objects
        LDFLAGS    += -flto=auto -fuse-linker-plugin
    endif
endif

ifeq ($(UNAME), Darwin)
    ifeq ($(debug), 0)
        STRIP = strip -x

        LDFLAGS += -Wl,-dead_strip

        LTO_CFLAGS += -flto
        LDFLAGS    += -flto
    endif
endif

##############################################################################
# Directory where generated files are stored

out_dir_base ?= Out

ifeq ($(debug), 0)
    out_dir_config = release
else
    out_dir_config = debug
endif

out_dir = $(out_dir_base)/$(out_dir_config)

##############################################################################
# Functions for constructing target paths

ifeq ($(UNAME), Windows)
    o_suffix = obj
else
    o_suffix = o
endif

OBJ_FROM_SRC = $(addsuffix .$(o_suffix), $(addprefix $(out_dir)/,$(basename $(notdir $1))))

ifeq ($(UNAME), Windows)
    exe_suffix = .exe
else
    exe_suffix =
endif

CMDLINE_PATH = $(out_dir)/$1$(exe_suffix)

##############################################################################
# Rules

default: $(foreach target, $(targets), $(call CMDLINE_PATH,$(target)))

.PHONY: default

all: default $(foreach test, $(tests), $(call CMDLINE_PATH,$(test)))

clean:
	rm -rf $(out_dir)

.PHONY: clean

$(out_dir_base):
	mkdir -p $@

$(out_dir): | $(out_dir_base)
	mkdir -p $@

define CC_RULE
$$(call OBJ_FROM_SRC,$1): $1 | $$(out_dir)
	$$(CC) $$(CFLAGS) $$(WFLAGS) $$(LTO_CFLAGS) -c $$(call COMPILER_OUTPUT,$$@) $$<
endef

TARGET_SOURCES = $($1_src_files)

all_src_files = $(sort $(foreach target, $(targets) $(tests), $(call TARGET_SOURCES,$(target))))

$(foreach source, $(all_src_files), $(eval $(call CC_RULE,$(source))))

define LINK_RULE
$$(call CMDLINE_PATH,$1): $$(call OBJ_FROM_SRC,$2)
	$$(LINK) $$(call LINKER_OUTPUT,$$@) $$^ $$(LDFLAGS)
ifdef STRIP
	$$(STRIP) $$@
endif
endef

$(foreach target, $(targets) $(tests), $(eval $(call LINK_RULE,$(target),$(call TARGET_SOURCES,$(target)))))

test: $(tests)

.PHONY: test $(tests)

define RUN_TEST
$1: $$(call CMDLINE_PATH,$1)
	MallocNanoZone=0 $$<
endef

$(foreach test, $(tests), $(eval $(call RUN_TEST,$(test))))

##############################################################################
# Dependency files

dep_files = $(addprefix $(out_dir)/, $(addsuffix .d, $(basename $(notdir $(all_src_files)))))

-include $(dep_files)
