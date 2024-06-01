# default settings
PREFIX=/usr/local
OPT=-O0 -g
WARN=-Wall -Wextra
SUBDIRS=examples

# settings from 'configure', may overwrite defaults
-include Makefile.config

LDFLAGS=$(OPT)
IFLAGS=-I$(SDIR)include -I$(SDIR)src -I.
LDLIBS=-ldl -ldyn_psets

SRCS = $(wildcard $(SDIR)src/*.c)
ifdef USE_TCP
SRCS += $(wildcard $(SDIR)src/backends/tcp/*.c)
IFLAGS += $(TCP_INC)
LDLIBS += $(TCP_LIBS)
endif
HEADERS = $(wildcard $(SDIR)include/*.h $(SDIR)include/laik/*.h)
OBJS = $(SRCS:$(SDIR)%.c=%.o)

CFLAGS=$(OPT) $(WARN) $(DEFS) $(IFLAGS) -std=gnu99 -fPIC

# instruct GCC to produce dependency files
DEPS = $(SRCS:$(SDIR)%.c=%.d)
CFLAGS+=-MMD -MP

# MPICC always must be set, even if MPI not found (then use regular C compiler)
MPICC ?= $(CC)
LAIKLIB = liblaik.so

# build targets
.PHONY: $(SUBDIRS) force

all: $(LAIKLIB) $(SUBDIRS) testbins README.md

# version information for first line of LAIK_LOG=2
# only trigger compile if git revision changes
git-version.h: force
	sh $(SDIR)update-version.sh

external/MQTT: $(LAIKLIB)
	cd external/MQTT && $(MAKE)

external/simple: $(LAIKLIB)
	cd external/simple && $(MAKE)

src/%.o: $(SDIR)src/%.c
	$(CC) -c $(CFLAGS) -c $< -o $@

src/revinfo.o: git-version.h

src/backend-mpi.o: $(SDIR)src/backend-mpi.c
	$(MPICC) $(CFLAGS) -c -o src/backend-mpi.o $(SDIR)src/backend-mpi.c

src/backend-mpi-dynamic.o: $(SDIR)src/backend-mpi-dynamic.c
	$(MPICC) $(CFLAGS) -c -o src/backend-mpi-dynamic.o $(SDIR)src/backend-mpi-dynamic.c

$(LAIKLIB): $(OBJS)
	$(MPICC) $(CFLAGS) -shared -o $(abspath $(LAIKLIB)) $(OBJS) $(LDLIBS)

examples: $(LAIKLIB)
	cd examples && $(MAKE)

examples/c++: $(LAIKLIB)
	cd examples/c++ && $(MAKE)


# tests
test: examples testbins
	$(MAKE) -C tests

testbins: $(LAIKLIB)
	+$(MAKE) testbins -C tests

# tidy
tidy:
	make clean
	bear make test
	git ls-files '*.c' | xargs -P 1 -I{} clang-tidy {};

tidy-clean: clean
	rm -f compile_commands.json

# clean targets
SUBDIRS_CLEAN=$(addprefix clean_, $(SUBDIRS)) clean_tests
.PHONY: $(SUBDIRS_CLEAN)

clean: clean_laik $(SUBDIRS_CLEAN)

clean_laik:
	rm -f *~ *.o git-version.h $(OBJS) $(DEPS) $(LAIKLIB)

$(SUBDIRS_CLEAN): clean_%:
	+$(MAKE) clean -C $*


# install targets
install: install_laik

install_laik: $(LAIKLIB) $(HEADERS)
	cp $(wildcard $(SDIR)include/*.h) $(PREFIX)/include
	mkdir -p $(PREFIX)/include/laik
	cp $(wildcard $(SDIR)include/laik/*.h) $(PREFIX)/include/laik
	#mkdir -p $(PREFIX)/include/interface
	#cp $(wildcard $(SDIR)include/interface/*.h) $(PREFIX)/include/interface
	mkdir -p $(PREFIX)/lib
	cp $(LAIKLIB) $(PREFIX)/lib

# install targets
uninstall: uninstall_laik

uninstall_laik:
	rm -rf $(PREFIX)/include/laik
	rm -f $(PREFIX)/include/laik.h
	rm -f $(PREFIX)/include/laik-internal.h
	rm -f $(PREFIX)/include/laik-backend-*.h
	rm -f $(PREFIX)/lib/liblaik.*

README.md: $(SDIR)README.in $(SDIR)examples/README-example.c
	sed -e '/EXAMPLECODE/ {r $(SDIR)examples/README-example.c' -e 'd' -e '}' $(SDIR)README.in > README.md

# include previously generated dependency rules if existing
-include $(DEPS)
