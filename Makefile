# SPDX-License-Identifier: MIT
# Copyright (c) 2022 Chris Dragan

##############################################################################
# Targets and sources

targets += minify
minify_src_files += arith_decode.c
minify_src_files += arith_encode.c
minify_src_files += bit_emit.c
minify_src_files += bit_stream.c
minify_src_files += buffer.c
minify_src_files += exe_pe.c
minify_src_files += find_repeats.c
minify_src_files += load_file.c
minify_src_files += lz_decompress.c
minify_src_files += lza_compress.c
minify_src_files += lza_decompress.c
minify_src_files += minify.c

targets += arith_encoder
arith_encoder_src_files += arith_decode.c
arith_encoder_src_files += arith_encode_file.c
arith_encoder_src_files += arith_encode.c
arith_encoder_src_files += bit_emit.c
arith_encoder_src_files += bit_stream.c
arith_encoder_src_files += buffer.c
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

tests += test_bit_stream
test_bit_stream_src_files += bit_emit.c
test_bit_stream_src_files += bit_stream.c
test_bit_stream_src_files += test_bit_stream.c

loaders += pe_load_imports
pe_load_imports_sources += pe_load_imports.c

loaders += pe_arith_decode
pe_arith_decode_sources += arith_decode.c
pe_arith_decode_sources += bit_stream.c
pe_arith_decode_sources += pe_arith_decode.c

loaders += pe_lz_decompress
pe_lz_decompress_sources += bit_stream.c
pe_lz_decompress_sources += lz_decompress.c
pe_lz_decompress_sources += pe_lz_decompress.c

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
        CFLAGS  += -O2 -Oi -DNDEBUG -MT
        ifneq ($(CC), clang-cl.exe)
            CFLAGS += -GL
        endif
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

    COMPILER_OUTPUT = -Fo$1
    LINKER_OUTPUT   = -out:$1

    STUB_CFLAGS += -O1 -Oi -DNDEBUG -MT
    STUB_CFLAGS += -DNOSTDLIB -D_NO_CRT_STDIO_INLINE -Zc:threadSafeInit- -GS- -Gs9999999
    STUB_CFLAGS += -nologo
    STUB_CFLAGS += -GR- -TP -EHa- -FS

    ifneq ($(CC), clang-cl.exe)
        STUB_CFLAGS += -GL-
    endif

    STUB_LDFLAGS += -nodefaultlib -stack:0x100000,0x100000
    STUB_LDFLAGS += -nologo
    STUB_LDFLAGS += -subsystem:windows
    STUB_LDFLAGS += -entry:loader

    DISASM_COMMAND = dumpbin -disasm -section:.text -nologo -out:$1 $2
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

    STUB_CFLAGS += -fvisibility=hidden
    STUB_CFLAGS += -fPIC
    STUB_CFLAGS += -MD
    STUB_CFLAGS += -Os -DNDEBUG
    STUB_CFLAGS += -fomit-frame-pointer
    STUB_CFLAGS += -fno-stack-check -fno-stack-protector
    STUB_CFLAGS += -ffunction-sections -fdata-sections

    STUB_LDFLAGS += -ffunction-sections -fdata-sections
    STUB_LDFLAGS += -Wl,-e -Wl,_loader
endif

ifeq ($(UNAME), Linux)
    ifeq ($(debug), 0)
        STRIP = strip

        LDFLAGS += -Wl,--gc-sections -Wl,--as-needed

        LTO_CFLAGS += -flto -fno-fat-lto-objects
        LDFLAGS    += -flto=auto -fuse-linker-plugin
    endif
    STUB_LDFLAGS += -Wl,--gc-sections -Wl,--as-needed
    STUB_STRIP = strip
    DISASM_COMMAND = objdump -d -M intel $2 > $1
endif

ifeq ($(UNAME), Darwin)
    ifeq ($(debug), 0)
        STRIP = strip -x

        LDFLAGS += -Wl,-dead_strip

        LTO_CFLAGS += -flto
        LDFLAGS    += -flto
    endif
    STUB_CFLAGS  += -flto
    STUB_LDFLAGS += -flto
    STUB_LDFLAGS += -Wl,-dead_strip
    STUB_STRIP = strip -x
    DISASM_COMMAND = objdump -d --x86-asm-syntax=intel $2 > $1
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

out_loader_dir = $(out_dir_base)/loaders

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
# Stubs

define STUB_RULE
default: $$(out_loader_dir)/$1.asm

$$(out_loader_dir)/$1.asm: $$(out_loader_dir)/$1$$(exe_suffix)
	$$(call DISASM_COMMAND,$$@,$$^)

$$(out_loader_dir)/$1$$(exe_suffix): $$(addprefix $(out_loader_dir)/,$$(addsuffix .$$(o_suffix),$$(basename $$($1_sources))))
	$$(LINK) $$(call LINKER_OUTPUT,$$@) $$^ $$(STUB_LDFLAGS)
ifdef STUB_STRIP
	$$(STUB_STRIP) $$@
endif

all_loader_sources += $$($1_sources)
endef

define STUB_CC_RULE
$$(out_loader_dir)/$$(basename $1).$$(o_suffix): $1 | $$(out_loader_dir)
	$$(CC) $$(STUB_CFLAGS) $$(WFLAGS) -c $$(call COMPILER_OUTPUT,$$@) $$<
endef

$(out_loader_dir):
	mkdir -p $@

$(foreach loader, $(loaders), $(eval $(call STUB_RULE,$(loader))))

$(foreach src, $(sort $(all_loader_sources)), $(eval $(call STUB_CC_RULE,$(src))))

##############################################################################
# Dependency files

dep_files = $(addprefix $(out_dir)/, $(addsuffix .d, $(basename $(notdir $(all_src_files)))))

-include $(dep_files)
