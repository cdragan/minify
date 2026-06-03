# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Chris Dragan

##############################################################################
# Targets and sources

targets += minify
minify_src_files += arith_model.c
minify_src_files += bit_emit.c
minify_src_files += bit_stream.c
minify_src_files += buffer.c
minify_src_files += exe_macho.c
minify_src_files += exe_pe.c
minify_src_files += find_repeats.c
minify_src_files += load_file.c
minify_src_files += lza_compress.c
minify_src_files += lza_decompress.c
minify_src_files += macho_sign.c
minify_src_files += map_exe.c
minify_src_files += map_macho.c
minify_src_files += map_pe.c
minify_src_files += minify.c
minify_src_files += sha256.c

tests += test_repeats
test_repeats_src_files += test_repeats.c
test_repeats_src_files += find_repeats.c

tests += test_bit_stream
test_bit_stream_src_files += bit_emit.c
test_bit_stream_src_files += bit_stream.c
test_bit_stream_src_files += test_bit_stream.c

tests += test_sha256
test_sha256_src_files += sha256.c
test_sha256_src_files += test_sha256.c

loaders += pe_load_imports
pe_load_imports_sources += pe_load_imports.c

loaders += pe_lza_decompress
pe_lza_decompress_sources += arith_model.c
pe_lza_decompress_sources += bit_stream.c
pe_lza_decompress_sources += lza_decompress.c
pe_lza_decompress_sources += pe_lza_decompress.c

##############################################################################
# Determine target OS

UNAME = $(shell uname -s)
ARCH ?= $(shell uname -m)

ifneq (,$(filter CYGWIN% MINGW% MSYS%, $(UNAME)))
    # Note: Still use cl.exe on Windows
    UNAME = Windows
endif

# macho_loader uses arm64 syscall asm; it only compiles on macOS arm64.
ifeq ($(UNAME)_$(ARCH), Darwin_arm64)
    loaders += macho_loader
    macho_loader_sources += arith_model.c
    macho_loader_sources += bit_stream.c
    macho_loader_sources += lza_decompress.c
    macho_loader_sources += macho_loader.c
endif

##############################################################################
# Default build flags

# Debug vs release
debug   ?= 1
release ?= $(if $(filter 1,$(debug)),0,1)

##############################################################################
# Compiler flags

ifeq ($(UNAME), Windows)
    WFLAGS += -W3

    ifeq ($(release), 1)
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
    LDFLAGS += -map:$(basename $@).map

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

    ifeq ($(CC), clang-cl.exe)
        STUB_CFLAGS += -fno-builtin
        ifeq ($(ARCH), x86)
            STUB_CFLAGS += -m32
        endif
    endif

    STUB_LDFLAGS += -nodefaultlib -stack:0x100000,0x100000
    STUB_LDFLAGS += -nologo
    STUB_LDFLAGS += -subsystem:windows
    STUB_LDFLAGS += -entry:loader
    STUB_LDFLAGS += -merge:.data=.text
    STUB_LDFLAGS += -Brepro

    DISASM_COMMAND = dumpbin -disasm -section:.text -nologo -out:$1 $2
else
    WFLAGS += -Wall -Wextra -Wno-unused-parameter -Wunused -Wno-missing-field-initializers
    WFLAGS += -Wshadow -Wformat=2 -Wconversion -Wdouble-promotion
    WFLAGS += -Werror

    CFLAGS += -fvisibility=hidden
    CFLAGS += -fPIC
    CFLAGS += -MD

    ifeq ($(release), 1)
        CFLAGS += -DNDEBUG -O3
        CFLAGS += -fomit-frame-pointer
        CFLAGS += -fno-stack-check -fno-stack-protector
        # For C++:
        #CFLAGS += -fno-threadsafe-statics

        CFLAGS  += -ffunction-sections -fdata-sections
        LDFLAGS += -ffunction-sections -fdata-sections
    else
        CFLAGS  += -fsanitize=address,undefined
        LDFLAGS += -fsanitize=address,undefined

        CFLAGS += -O0 -g
    endif

    LINK = $(CC)

    COMPILER_OUTPUT = -o $1
    LINKER_OUTPUT   = -o $1

    STUB_CFLAGS += -fvisibility=hidden
    STUB_CFLAGS += -fPIC
    STUB_CFLAGS += -MD
    STUB_CFLAGS += -DNDEBUG
    STUB_CFLAGS += -fomit-frame-pointer
    STUB_CFLAGS += -fno-stack-check -fno-stack-protector
    STUB_CFLAGS += -ffunction-sections -fdata-sections

    STUB_LDFLAGS += -ffunction-sections -fdata-sections
    STUB_LDFLAGS += -Wl,-e -Wl,_loader
endif

ifeq ($(UNAME), Linux)
    ifeq ($(release), 1)
        STRIP = strip

        LDFLAGS += -Wl,--gc-sections -Wl,--as-needed

        LTO_CFLAGS += -flto -fno-fat-lto-objects
        LDFLAGS    += -flto=auto -fuse-linker-plugin
    endif
    LDFLAGS      += -Wl,-Map=$(basename $@).map
    STUB_CFLAGS  += -Os
    STUB_LDFLAGS += -Wl,--gc-sections -Wl,--as-needed
    STUB_STRIP = strip
    DISASM_COMMAND = objdump -d -M intel $2 > $1
endif

ifeq ($(UNAME), Darwin)
    ifeq ($(release), 1)
        STRIP = strip -x

        LDFLAGS += -Wl,-dead_strip

        LTO_CFLAGS += -flto
        LDFLAGS    += -flto
    else
        export MallocNanoZone=0
    endif
    LDFLAGS      += -Wl,-map,$(basename $@).map
    STUB_CFLAGS  += -Oz
    STUB_CFLAGS  += -fno-asynchronous-unwind-tables -fno-unwind-tables
    STUB_CFLAGS  += -flto
    STUB_LDFLAGS += -flto
    STUB_LDFLAGS += -Wl,-dead_strip
    STUB_LDFLAGS += -Wl,-no_uuid
    STUB_STRIP = strip -x
    DISASM_COMMAND = objdump -d --x86-asm-syntax=intel $2 > $1
endif

##############################################################################
# Directory where generated files are stored

out_dir_base ?= Out

ifeq ($(release), 1)
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

default: build test

build: $(foreach target, $(targets), $(call CMDLINE_PATH,$(target)))

build_all: default $(foreach test, $(tests), $(call CMDLINE_PATH,$(test)))

clean:
	rm -rf $(out_dir)

.PHONY: build default build_all clean

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
	$$<
endef

$(foreach test, $(tests), $(eval $(call RUN_TEST,$(test))))

##############################################################################
# Mach-O test

macho_test_dir = $(out_dir)/macho_test

macho_compress_test: $(call CMDLINE_PATH,minify)
	rm -rf $(macho_test_dir)
	mkdir -p $(macho_test_dir)
	cp loaders/macos/arm64/macho_loader $(macho_test_dir)/in_arm64
	$< $(macho_test_dir)/in_arm64
	test -s $(macho_test_dir)/mini.in_arm64

test: macho_compress_test
.PHONY: macho_compress_test

ifeq ($(UNAME)_$(ARCH), Darwin_arm64)

macho_run_test: macho_compress_test
	cp $(call CMDLINE_PATH,minify) $(macho_test_dir)/probe_in
	cp $(call CMDLINE_PATH,minify) $(macho_test_dir)/inner_in
	$(call CMDLINE_PATH,minify) $(macho_test_dir)/probe_in
	$(macho_test_dir)/mini.probe_in $(macho_test_dir)/inner_in
	test -s $(macho_test_dir)/mini.inner_in

test: macho_run_test
.PHONY: macho_run_test

endif

##############################################################################
# PE test

pe_test_dir = $(out_dir)/pe_test

pe_compress_test: $(call CMDLINE_PATH,minify)
	rm -rf $(pe_test_dir)
	mkdir -p $(pe_test_dir)
	cp loaders/windows/x86/pe_load_imports.exe $(pe_test_dir)/in_x86.exe
	cp loaders/windows/x64/pe_load_imports.exe $(pe_test_dir)/in_x64.exe
	$< $(pe_test_dir)/in_x86.exe
	$< $(pe_test_dir)/in_x64.exe
	test -s $(pe_test_dir)/mini.in_x86.exe
	test -s $(pe_test_dir)/mini.in_x64.exe

test: pe_compress_test
.PHONY: pe_compress_test

#ifeq ($(UNAME), Windows)
#
#pe_run_test: pe_compress_test
#	cp $(call CMDLINE_PATH,minify) $(pe_test_dir)/probe_in.exe
#	cp $(call CMDLINE_PATH,minify) $(pe_test_dir)/inner_in.exe
#	$(call CMDLINE_PATH,minify) $(pe_test_dir)/probe_in.exe
#	$(pe_test_dir)/mini.probe_in.exe $(pe_test_dir)/inner_in.exe
#	test -s $(pe_test_dir)/mini.inner_in.exe
#
#test: pe_run_test
#.PHONY: pe_run_test
#
#endif

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

build_loaders: $(foreach loader, $(loaders), $(out_loader_dir)/$(loader)$(exe_suffix))
.PHONY: build_loaders

$(foreach src, $(sort $(all_loader_sources)), $(eval $(call STUB_CC_RULE,$(src))))

##############################################################################
# Dependency files

dep_files = $(addprefix $(out_dir)/, $(addsuffix .d, $(basename $(notdir $(all_src_files)))))
loader_dep_files = $(addprefix $(out_loader_dir)/, $(addsuffix .d, $(sort $(basename $(all_loader_sources)))))

-include $(dep_files)
-include $(loader_dep_files)
