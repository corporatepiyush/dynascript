#
# DynaJS Javascript Engine
#
# Copyright (c) 2017-2021 Fabrice Bellard
# Copyright (c) 2017-2021 Charlie Gordon
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

ifeq ($(shell uname -s),Darwin)
CONFIG_DARWIN=y
endif
ifeq ($(shell uname -s),FreeBSD)
CONFIG_FREEBSD=y
endif
# Windows cross compilation from Linux
# May need to have libwinpthread*.dll alongside the executable
# (On Ubuntu/Debian may be installed with mingw-w64-x86-64-dev
# to /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll)
#CONFIG_WIN32=y
# use link time optimization (smaller and faster executables but slower build)
#CONFIG_LTO=y
# consider warnings as errors (for development)
#CONFIG_WERROR=y
# force 32 bit build on x86_64
#CONFIG_M32=y
# cosmopolitan build (see https://github.com/jart/cosmopolitan)
#CONFIG_COSMO=y

# installation directory
PREFIX?=/usr/local

# use the gprof profiler
#CONFIG_PROFILE=y
# use address sanitizer
#CONFIG_ASAN=y
# use memory sanitizer
#CONFIG_MSAN=y
# use UB sanitizer
#CONFIG_UBSAN=y
# use thread sanitizer
#CONFIG_TSAN=y

# TEST262 bootstrap config: commit id and shallow "since" parameter
TEST262_COMMIT?=5c8206929d81b2d3d727ca6aac56c18358c8d790
TEST262_SINCE?=2025-09-01

OBJDIR=.obj

ifdef CONFIG_ASAN
OBJDIR:=$(OBJDIR)/asan
endif
ifdef CONFIG_MSAN
OBJDIR:=$(OBJDIR)/msan
endif
ifdef CONFIG_UBSAN
OBJDIR:=$(OBJDIR)/ubsan
endif
ifdef CONFIG_TSAN
OBJDIR:=$(OBJDIR)/tsan
endif

ifdef CONFIG_DARWIN
# use clang instead of gcc
CONFIG_CLANG=y
CONFIG_DEFAULT_AR=y
endif
ifdef CONFIG_FREEBSD
# use clang instead of gcc
CONFIG_CLANG=y
CONFIG_DEFAULT_AR=y
CONFIG_LTO=
endif

ifdef CONFIG_WIN32
  ifdef CONFIG_M32
    CROSS_PREFIX?=i686-w64-mingw32-
  else
    CROSS_PREFIX?=x86_64-w64-mingw32-
  endif
  EXE=.exe
else ifdef MSYSTEM
  CONFIG_WIN32=y
  CROSS_PREFIX?=
  EXE=.exe
else
  CROSS_PREFIX?=
  EXE=
endif

ifdef CONFIG_CLANG
  HOST_CC=clang
  CC=$(CROSS_PREFIX)clang
  CFLAGS+=-g -Wall -MMD -MF $(OBJDIR)/$(@F).d
  CFLAGS += -Wextra
  CFLAGS += -Wno-sign-compare
  CFLAGS += -Wno-missing-field-initializers
  CFLAGS += -Wundef -Wuninitialized
  CFLAGS += -Wunused -Wno-unused-parameter
  CFLAGS += -Wwrite-strings
  CFLAGS += -Wchar-subscripts -funsigned-char
  CFLAGS += -MMD -MF $(OBJDIR)/$(@F).d
  ifdef CONFIG_DEFAULT_AR
    AR=$(CROSS_PREFIX)ar
  else
    ifdef CONFIG_LTO
      AR=$(CROSS_PREFIX)llvm-ar
    else
      AR=$(CROSS_PREFIX)ar
    endif
  endif
  LIB_FUZZING_ENGINE ?= "-fsanitize=fuzzer"
else ifdef CONFIG_COSMO
  CONFIG_LTO=
  HOST_CC=gcc
  CC=cosmocc
  # cosmocc does not correct support -MF
  CFLAGS=-g -Wall #-MMD -MF $(OBJDIR)/$(@F).d
  CFLAGS += -Wno-array-bounds -Wno-format-truncation
  AR=cosmoar
else
  HOST_CC=gcc
  CC=$(CROSS_PREFIX)gcc
  CFLAGS+=-g -Wall -MMD -MF $(OBJDIR)/$(@F).d
  CFLAGS += -Wno-array-bounds -Wno-format-truncation -Wno-infinite-recursion
  ifdef CONFIG_LTO
    AR=$(CROSS_PREFIX)gcc-ar
  else
    AR=$(CROSS_PREFIX)ar
  endif
endif
STRIP?=$(CROSS_PREFIX)strip
ifdef CONFIG_M32
CFLAGS+=-msse2 -mfpmath=sse # use SSE math for correct FP rounding
ifndef CONFIG_WIN32
CFLAGS+=-m32
LDFLAGS+=-m32
endif
endif
CFLAGS+=-std=gnu17 # pin C17 (gnu variant: computed-goto &&label / goto * need GNU extensions)
CFLAGS+=-fwrapv # ensure that signed overflows behave as expected
ifdef CONFIG_WERROR
CFLAGS+=-Werror
endif
DEFINES:=-D_GNU_SOURCE -DCONFIG_VERSION=\"$(shell cat VERSION)\"
ifdef CONFIG_WIN32
DEFINES+=-D__USE_MINGW_ANSI_STDIO # for standard snprintf behavior
endif
ifndef CONFIG_WIN32
ifeq ($(shell $(CC) -o /dev/null compat/test-closefrom.c 2>/dev/null && echo 1),1)
DEFINES+=-DHAVE_CLOSEFROM
endif
endif

CFLAGS+=$(DEFINES)
# repo root on the include path so dynajs.c's src/*.inc.c fragments can
# resolve project headers (e.g. dynajs-opcode.h) regardless of their subdir
CFLAGS+=-I.
CFLAGS_DEBUG=$(CFLAGS) -O0
CFLAGS_SMALL=$(CFLAGS) -Os
CFLAGS_OPT=$(CFLAGS) -O2
# opt-in local-CPU tuning for benchmarking; off by default so shipped builds
# stay portable. -mcpu=native picks up the host uarch (NEON/SVE widths, etc.).
ifdef CONFIG_NATIVE
CFLAGS_OPT+=-mcpu=native
CFLAGS_SMALL+=-mcpu=native
endif
CFLAGS_NOLTO:=$(CFLAGS_OPT)
ifdef CONFIG_COSMO
LDFLAGS+=-s # better to strip by default
else
LDFLAGS+=-g
endif
ifdef CONFIG_LTO
CFLAGS_SMALL+=-flto
CFLAGS_OPT+=-flto
LDFLAGS+=-flto
endif
ifdef CONFIG_PROFILE
CFLAGS+=-p
LDFLAGS+=-p
endif
ifdef CONFIG_ASAN
CFLAGS+=-fsanitize=address -fno-omit-frame-pointer
LDFLAGS+=-fsanitize=address -fno-omit-frame-pointer
endif
ifdef CONFIG_MSAN
CFLAGS+=-fsanitize=memory -fno-omit-frame-pointer
LDFLAGS+=-fsanitize=memory -fno-omit-frame-pointer
endif
ifdef CONFIG_UBSAN
CFLAGS+=-fsanitize=undefined -fno-omit-frame-pointer
LDFLAGS+=-fsanitize=undefined -fno-omit-frame-pointer
endif
ifdef CONFIG_TSAN
CFLAGS+=-fsanitize=thread -fno-omit-frame-pointer
LDFLAGS+=-fsanitize=thread -fno-omit-frame-pointer
endif
# route the runtime's backing allocator through secure-c-libs (opt-in).
# SCL_DIR points at the secure-c-libs checkout providing libscl.a.
SCL_DIR?=../secure-c-libs
ifdef CONFIG_SCL_ALLOC
CFLAGS+=-DCONFIG_SCL_ALLOC
EXTRA_LIBS+=$(SCL_DIR)/libscl.a
endif
# expose secure-c-libs modules to JS as scl:* native modules (opt-in).
ifdef CONFIG_SCL_MODULES
CFLAGS+=-DCONFIG_SCL_MODULES
# -I every secure-c-libs header directory
CFLAGS+=$(addprefix -I,$(sort $(dir $(wildcard $(SCL_DIR)/libs/*/*.h $(SCL_DIR)/libs/*/*/*.h))))
EXTRA_LIBS+=$(SCL_DIR)/libscl.a
# each family is active iff its binding file is present (an integrated module
# builds by default under CONFIG_SCL_MODULES); an explicit flag also works.
ifneq ($(or $(wildcard dynajs-scl-http.c),$(CONFIG_SCL_MODULE_HTTP)),)
CFLAGS+=-DCONFIG_SCL_MODULE_HTTP
endif
ifneq ($(or $(wildcard dynajs-scl-ml.c),$(CONFIG_SCL_MODULE_ML)),)
CFLAGS+=-DCONFIG_SCL_MODULE_ML
endif
ifneq ($(or $(wildcard dynajs-scl-docparse.c),$(CONFIG_SCL_MODULE_DOCPARSE)),)
CFLAGS+=-DCONFIG_SCL_MODULE_DOCPARSE
endif
ifneq ($(or $(wildcard dynajs-scl-compress.c),$(CONFIG_SCL_MODULE_COMPRESS)),)
CFLAGS+=-DCONFIG_SCL_MODULE_COMPRESS
endif
ifneq ($(or $(wildcard dynajs-scl-random.c),$(CONFIG_SCL_MODULE_RANDOM)),)
CFLAGS+=-DCONFIG_SCL_MODULE_RANDOM
endif
ifneq ($(or $(wildcard dynajs-scl-sort.c),$(CONFIG_SCL_MODULE_SORT)),)
CFLAGS+=-DCONFIG_SCL_MODULE_SORT
endif
ifneq ($(or $(wildcard dynajs-scl-search.c),$(CONFIG_SCL_MODULE_SEARCH)),)
CFLAGS+=-DCONFIG_SCL_MODULE_SEARCH
endif
ifneq ($(wildcard dynajs-scl-structures3.c),)
CFLAGS+=-DCONFIG_SCL_MODULE_STRUCTURES3
endif
endif
# in-repo native modules (self-contained, NO external deps). A family is active
# iff its dynajs-<family>.c is present. No -I/-l into any external tree.
ifdef CONFIG_NATIVE_MODULES
CFLAGS+=-DCONFIG_NATIVE_MODULES
ifneq ($(wildcard dynajs-random.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_RANDOM
endif
ifneq ($(wildcard dynajs-sort.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_SORT
endif
ifneq ($(wildcard dynajs-search.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_SEARCH
endif
ifneq ($(wildcard dynajs-compress.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_COMPRESS
endif
ifneq ($(wildcard dynajs-http.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_HTTP
endif
ifneq ($(wildcard dynajs-structures.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_STRUCTURES
endif
ifneq ($(wildcard dynajs-structures3.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_STRUCTURES3
endif
ifneq ($(wildcard dynajs-ml.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_ML
endif
ifneq ($(wildcard dynajs-docparse.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_DOCPARSE
endif
endif
ifdef CONFIG_WIN32
LDEXPORT=
else
LDEXPORT=-rdynamic
endif

ifndef CONFIG_COSMO
ifndef CONFIG_DARWIN
ifndef CONFIG_WIN32
CONFIG_SHARED_LIBS=y # building shared libraries is supported
endif
endif
endif

PROGS=dynajs$(EXE) dynajsc$(EXE) run-test262$(EXE)

ifneq ($(CROSS_PREFIX),)
DYNAJSC_CC=gcc
DYNAJSC=./host-dynajsc
PROGS+=$(DYNAJSC)
else
DYNAJSC_CC=$(CC)
DYNAJSC=./dynajsc$(EXE)
endif
PROGS+=libdynajs.a
ifdef CONFIG_LTO
PROGS+=libdynajs.lto.a
endif

# examples
ifeq ($(CROSS_PREFIX),)
ifndef CONFIG_ASAN
ifndef CONFIG_MSAN
ifndef CONFIG_UBSAN
PROGS+=examples/hello examples/test_fib
# no -m32 option in dynajsc
ifndef CONFIG_M32
ifndef CONFIG_WIN32
PROGS+=examples/hello_module
endif
endif
ifdef CONFIG_SHARED_LIBS
PROGS+=examples/fib.so examples/point.so
endif
endif
endif
endif
endif

all: $(OBJDIR) $(OBJDIR)/dynajs.check.o $(OBJDIR)/dynajs-cli.check.o $(PROGS)

DYNAJS_LIB_OBJS=$(OBJDIR)/dynajs.o $(OBJDIR)/dtoa.o $(OBJDIR)/libregexp.o $(OBJDIR)/libunicode.o $(OBJDIR)/cutils.o $(OBJDIR)/dynajs-libc.o

DYNAJS_OBJS=$(OBJDIR)/dynajs-cli.o $(OBJDIR)/repl.o $(DYNAJS_LIB_OBJS)
ifdef CONFIG_SCL_MODULES
# scl:* native module binding objects (each family's object added as it lands)
SCL_MODULE_OBJS=$(OBJDIR)/dynajs-scl.o $(OBJDIR)/dynajs-scl-structures.o
ifneq ($(wildcard dynajs-scl-structures-ext.c),)
SCL_MODULE_OBJS+=$(OBJDIR)/dynajs-scl-structures-ext.o
endif
ifneq ($(or $(wildcard dynajs-scl-http.c),$(CONFIG_SCL_MODULE_HTTP)),)
SCL_MODULE_OBJS+=$(OBJDIR)/dynajs-scl-http.o
endif
ifneq ($(or $(wildcard dynajs-scl-ml.c),$(CONFIG_SCL_MODULE_ML)),)
SCL_MODULE_OBJS+=$(OBJDIR)/dynajs-scl-ml.o
endif
ifneq ($(or $(wildcard dynajs-scl-docparse.c),$(CONFIG_SCL_MODULE_DOCPARSE)),)
SCL_MODULE_OBJS+=$(OBJDIR)/dynajs-scl-docparse.o
endif
ifneq ($(or $(wildcard dynajs-scl-compress.c),$(CONFIG_SCL_MODULE_COMPRESS)),)
SCL_MODULE_OBJS+=$(OBJDIR)/dynajs-scl-compress.o
endif
ifneq ($(or $(wildcard dynajs-scl-random.c),$(CONFIG_SCL_MODULE_RANDOM)),)
SCL_MODULE_OBJS+=$(OBJDIR)/dynajs-scl-random.o
endif
ifneq ($(or $(wildcard dynajs-scl-sort.c),$(CONFIG_SCL_MODULE_SORT)),)
SCL_MODULE_OBJS+=$(OBJDIR)/dynajs-scl-sort.o
endif
ifneq ($(or $(wildcard dynajs-scl-search.c),$(CONFIG_SCL_MODULE_SEARCH)),)
SCL_MODULE_OBJS+=$(OBJDIR)/dynajs-scl-search.o
endif
ifneq ($(wildcard dynajs-scl-structures3.c),)
SCL_MODULE_OBJS+=$(OBJDIR)/dynajs-scl-structures3.o
endif
DYNAJS_OBJS+=$(SCL_MODULE_OBJS)
endif
ifdef CONFIG_NATIVE_MODULES
# in-repo native module objects (framework + each present family)
NAT_MODULE_OBJS=$(OBJDIR)/dynajs-nat.o
ifneq ($(wildcard dynajs-random.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dynajs-random.o
endif
ifneq ($(wildcard dynajs-sort.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dynajs-sort.o
endif
ifneq ($(wildcard dynajs-search.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dynajs-search.o
endif
ifneq ($(wildcard dynajs-compress.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dynajs-compress.o
endif
ifneq ($(wildcard dynajs-http.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dynajs-http.o
endif
ifneq ($(wildcard dynajs-structures.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dynajs-structures.o
endif
ifneq ($(wildcard dynajs-structures3.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dynajs-structures3.o
endif
ifneq ($(wildcard dynajs-ml.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dynajs-ml.o
endif
ifneq ($(wildcard dynajs-docparse.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dynajs-docparse.o
endif
DYNAJS_OBJS+=$(NAT_MODULE_OBJS)
endif

HOST_LIBS=-lm -ldl -lpthread
LIBS=-lm -lpthread
ifndef CONFIG_WIN32
LIBS+=-ldl
endif
LIBS+=$(EXTRA_LIBS)

$(OBJDIR):
	mkdir -p $(OBJDIR) $(OBJDIR)/examples $(OBJDIR)/tests

dynajs$(EXE): $(DYNAJS_OBJS)
	$(CC) $(LDFLAGS) $(LDEXPORT) -o $@ $^ $(LIBS)

dynajs-debug$(EXE): $(patsubst %.o, %.debug.o, $(DYNAJS_OBJS))
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

dynajsc$(EXE): $(OBJDIR)/dynajsc.o $(DYNAJS_LIB_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

fuzz_eval: $(OBJDIR)/fuzz_eval.o $(OBJDIR)/fuzz_common.o libdynajs.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_eval $(LIB_FUZZING_ENGINE)

fuzz_compile: $(OBJDIR)/fuzz_compile.o $(OBJDIR)/fuzz_common.o libdynajs.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_compile $(LIB_FUZZING_ENGINE)

fuzz_regexp: $(OBJDIR)/fuzz_regexp.o $(OBJDIR)/libregexp.fuzz.o $(OBJDIR)/cutils.fuzz.o $(OBJDIR)/libunicode.fuzz.o
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_regexp $(LIB_FUZZING_ENGINE)

# reader targets drive JS_ParseJSON / JS_ReadObject on the raw buffer; they
# do not use fuzz_common's test_one_input_init
fuzz_json: $(OBJDIR)/fuzz_json.o libdynajs.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_json $(LIB_FUZZING_ENGINE)

fuzz_bytecode: $(OBJDIR)/fuzz_bytecode.o libdynajs.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_bytecode $(LIB_FUZZING_ENGINE)

fuzz_module_export: $(OBJDIR)/fuzz_module_export.o libdynajs.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_module_export $(LIB_FUZZING_ENGINE)

libfuzzer: fuzz_eval fuzz_compile fuzz_regexp fuzz_json fuzz_bytecode fuzz_module_export

ifneq ($(CROSS_PREFIX),)

$(DYNAJSC): $(OBJDIR)/dynajsc.host.o \
    $(patsubst %.o, %.host.o, $(DYNAJS_LIB_OBJS))
	$(HOST_CC) $(LDFLAGS) -o $@ $^ $(HOST_LIBS)

endif #CROSS_PREFIX

DYNAJSC_DEFINES:=-DCONFIG_CC=\"$(DYNAJSC_CC)\" -DCONFIG_PREFIX=\"$(PREFIX)\"
ifdef CONFIG_LTO
DYNAJSC_DEFINES+=-DCONFIG_LTO
endif
DYNAJSC_HOST_DEFINES:=-DCONFIG_CC=\"$(HOST_CC)\" -DCONFIG_PREFIX=\"$(PREFIX)\"

$(OBJDIR)/dynajsc.o: CFLAGS+=$(DYNAJSC_DEFINES)
$(OBJDIR)/dynajsc.host.o: CFLAGS+=$(DYNAJSC_HOST_DEFINES)

ifdef CONFIG_LTO
LTOEXT=.lto
else
LTOEXT=
endif

libdynajs$(LTOEXT).a: $(DYNAJS_LIB_OBJS)
	$(AR) rcs $@ $^

ifdef CONFIG_LTO
libdynajs.a: $(patsubst %.o, %.nolto.o, $(DYNAJS_LIB_OBJS))
	$(AR) rcs $@ $^
endif # CONFIG_LTO

libdynajs.fuzz.a: $(patsubst %.o, %.fuzz.o, $(DYNAJS_LIB_OBJS))
	$(AR) rcs $@ $^

repl.c: $(DYNAJSC) repl.js
	$(DYNAJSC) -s -c -o $@ -m repl.js

ifneq ($(wildcard unicode/UnicodeData.txt),)
$(OBJDIR)/libunicode.o $(OBJDIR)/libunicode.nolto.o: libunicode-table.h

libunicode-table.h: unicode_gen
	./unicode_gen unicode $@
endif

run-test262$(EXE): $(OBJDIR)/run-test262.o $(DYNAJS_LIB_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

run-test262-debug: $(patsubst %.o, %.debug.o, $(OBJDIR)/run-test262.o $(DYNAJS_LIB_OBJS))
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# object suffix order: nolto

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS_OPT) -c -o $@ $<

$(OBJDIR)/fuzz_%.o: fuzz/fuzz_%.c | $(OBJDIR)
	$(CC) $(CFLAGS_OPT) -c -I. -o $@ $<

$(OBJDIR)/%.host.o: %.c | $(OBJDIR)
	$(HOST_CC) $(CFLAGS_OPT) -c -o $@ $<

$(OBJDIR)/%.pic.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS_OPT) -fPIC -DJS_SHARED_LIBRARY -c -o $@ $<

$(OBJDIR)/%.nolto.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS_NOLTO) -c -o $@ $<

$(OBJDIR)/%.debug.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS_DEBUG) -c -o $@ $<

$(OBJDIR)/%.fuzz.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS_OPT) -fsanitize=fuzzer-no-link -c -o $@ $<

$(OBJDIR)/%.check.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DCONFIG_CHECK_JSVALUE -c -o $@ $<

regexp_test: libregexp.c libunicode.c cutils.c
	$(CC) $(LDFLAGS) $(CFLAGS) -DTEST -o $@ libregexp.c libunicode.c cutils.c $(LIBS)

unicode_gen: $(OBJDIR)/unicode_gen.host.o $(OBJDIR)/cutils.host.o libunicode.c unicode_gen_def.h
	$(HOST_CC) $(LDFLAGS) $(CFLAGS) -o $@ $(OBJDIR)/unicode_gen.host.o $(OBJDIR)/cutils.host.o

clean:
	rm -f repl.c out.c
	rm -f *.a *.o *.d *~ unicode_gen regexp_test fuzz_eval fuzz_compile fuzz_regexp $(PROGS)
	rm -f hello.c test_fib.c
	rm -f examples/*.so tests/*.so
	rm -rf $(OBJDIR)/ *.dSYM/ dynajs-debug$(EXE)
	rm -rf run-test262-debug$(EXE)
	rm -f run_octane run_sunspider_like

install: all
	mkdir -p "$(DESTDIR)$(PREFIX)/bin"
	$(STRIP) dynajs$(EXE) dynajsc$(EXE)
	install -m755 dynajs$(EXE) dynajsc$(EXE) "$(DESTDIR)$(PREFIX)/bin"
	mkdir -p "$(DESTDIR)$(PREFIX)/lib/dynajs"
	install -m644 libdynajs.a "$(DESTDIR)$(PREFIX)/lib/dynajs"
ifdef CONFIG_LTO
	install -m644 libdynajs.lto.a "$(DESTDIR)$(PREFIX)/lib/dynajs"
endif
	mkdir -p "$(DESTDIR)$(PREFIX)/include/dynajs"
	install -m644 dynajs.h dynajs-libc.h "$(DESTDIR)$(PREFIX)/include/dynajs"

###############################################################################
# examples

# example of static JS compilation
HELLO_SRCS=examples/hello.js
HELLO_OPTS=-fno-string-normalize -fno-map -fno-promise -fno-typedarray \
           -fno-typedarray -fno-regexp -fno-json -fno-eval -fno-proxy \
           -fno-date -fno-module-loader

hello.c: $(DYNAJSC) $(HELLO_SRCS)
	$(DYNAJSC) -e $(HELLO_OPTS) -o $@ $(HELLO_SRCS)

examples/hello: $(OBJDIR)/hello.o $(DYNAJS_LIB_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# example of static JS compilation with modules
HELLO_MODULE_SRCS=examples/hello_module.js
HELLO_MODULE_OPTS=-fno-string-normalize -fno-map -fno-typedarray \
           -fno-typedarray -fno-regexp -fno-json -fno-eval -fno-proxy \
           -fno-date -m
examples/hello_module: $(DYNAJSC) libdynajs$(LTOEXT).a $(HELLO_MODULE_SRCS)
	$(DYNAJSC) $(HELLO_MODULE_OPTS) -o $@ $(HELLO_MODULE_SRCS)

# use of an external C module (static compilation)

test_fib.c: $(DYNAJSC) examples/test_fib.js
	$(DYNAJSC) -e -M examples/fib.so,fib -m -o $@ examples/test_fib.js

examples/test_fib: $(OBJDIR)/test_fib.o $(OBJDIR)/examples/fib.o libdynajs$(LTOEXT).a
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

examples/fib.so: $(OBJDIR)/examples/fib.pic.o
	$(CC) $(LDFLAGS) -shared -o $@ $^

examples/point.so: $(OBJDIR)/examples/point.pic.o
	$(CC) $(LDFLAGS) -shared -o $@ $^

###############################################################################
# documentation

DOCS=doc/dynajs.pdf doc/dynajs.html

build_doc: $(DOCS)

clean_doc:
	rm -f $(DOCS)

doc/version.texi: VERSION
	@echo "@set VERSION `cat $<`" > $@

doc/%.pdf: doc/%.texi doc/version.texi
	texi2pdf --clean -o $@ -q $<

doc/%.html.pre: doc/%.texi doc/version.texi
	makeinfo --html --no-headers --no-split --number-sections -o $@ $<

doc/%.html: doc/%.html.pre
	sed -e 's|</style>|</style>\n<meta name="viewport" content="width=device-width, initial-scale=1.0">|' < $< > $@

###############################################################################
# tests

ifdef CONFIG_SHARED_LIBS
test: tests/bjson.so examples/point.so
endif

test: dynajs$(EXE)
	$(WINE) ./dynajs$(EXE) tests/test_closure.js
	$(WINE) ./dynajs$(EXE) tests/test_language.js
	$(WINE) ./dynajs$(EXE) --std tests/test_builtin.js
	$(WINE) ./dynajs$(EXE) tests/test_modern.js
	$(WINE) ./dynajs$(EXE) tests/test_disposable.js
	$(WINE) ./dynajs$(EXE) tests/test_meta.js
	$(WINE) ./dynajs$(EXE) tests/test_optimizer.js
	$(WINE) ./dynajs$(EXE) tests/test_loop.js
	$(WINE) ./dynajs$(EXE) tests/test_bigint.js
	$(WINE) ./dynajs$(EXE) tests/test_cyclic_import.js
	$(WINE) ./dynajs$(EXE) tests/test_worker.js
ifndef CONFIG_WIN32
	$(WINE) ./dynajs$(EXE) tests/test_std.js
	$(WINE) ./dynajs$(EXE) tests/test_rw_handler.js
endif
ifdef CONFIG_SHARED_LIBS
	$(WINE) ./dynajs$(EXE) tests/test_bjson.js
	$(WINE) ./dynajs$(EXE) examples/test_point.js
endif

stats: dynajs$(EXE)
	$(WINE) ./dynajs$(EXE) -qd

microbench: dynajs$(EXE)
	$(WINE) ./dynajs$(EXE) --std tests/microbench.js

ifeq ($(wildcard test262/features.txt),)
test2-bootstrap:
	git clone --single-branch --shallow-since=$(TEST262_SINCE) https://github.com/tc39/test262.git
	(cd test262 && git checkout -q $(TEST262_COMMIT) && patch -p1 < ../tests/test262.patch && cd ..)
else
test2-bootstrap:
	(cd test262 && git fetch && git reset --hard $(TEST262_COMMIT) && patch -p1 < ../tests/test262.patch && cd ..)
endif

ifeq ($(wildcard test262o/tests.txt),)
test2o test2o-update:
	@echo test262o tests not installed
else
# ES5 tests (obsolete)
test2o: run-test262
	time ./run-test262 -t -m -c test262o.conf

test2o-update: run-test262
	./run-test262 -t -u -c test262o.conf -T 1
endif

ifeq ($(wildcard test262/features.txt),)
test2 test2-update test2-default test2-check:
	@echo test262 tests not installed
else
# Test262 tests
test2-default: run-test262
	time ./run-test262 -t -m -c test262.conf

test2: run-test262
	time ./run-test262 -t -m -c test262.conf -a

test2-update: run-test262
	./run-test262 -t -u -c test262.conf -a

test2-check: run-test262
	time ./run-test262 -t -m -c test262.conf -E -a
endif

testall: all test microbench test2o test2

testall-complete: testall

node-test:
	node tests/test_closure.js
	node tests/test_language.js
	node tests/test_builtin.js
	node tests/test_loop.js
	node tests/test_bigint.js

node-microbench:
	node tests/microbench.js -s microbench-node.txt
	node --jitless tests/microbench.js -s microbench-node-jitless.txt

bench-v8: dynajs
	make -C tests/bench-v8
	./dynajs -d tests/bench-v8/combined.js

node-bench-v8:
	make -C tests/bench-v8
	node --jitless tests/bench-v8/combined.js

tests/bjson.so: $(OBJDIR)/tests/bjson.pic.o
	$(CC) $(LDFLAGS) -shared -o $@ $^ $(LIBS)

BENCHMARKDIR=../dynajs-benchmarks

run_sunspider_like: $(BENCHMARKDIR)/run_sunspider_like.c
	$(CC) $(CFLAGS) $(LDFLAGS) -DNO_INCLUDE_DIR -I. -o $@ $< libdynajs$(LTOEXT).a $(LIBS)

run_octane: $(BENCHMARKDIR)/run_octane.c
	$(CC) $(CFLAGS) $(LDFLAGS) -DNO_INCLUDE_DIR -I. -o $@ $< libdynajs$(LTOEXT).a $(LIBS)

benchmarks: run_sunspider_like run_octane
	./run_sunspider_like $(BENCHMARKDIR)/kraken-1.0/
	./run_sunspider_like $(BENCHMARKDIR)/kraken-1.1/
	./run_sunspider_like $(BENCHMARKDIR)/sunspider-1.0/
	./run_octane $(BENCHMARKDIR)/

-include $(wildcard $(OBJDIR)/*.d)
