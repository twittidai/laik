# defaults: can be overwritten by Makefile.config
# run 'configure' to generate Makefile.config

PREFIX=/usr/local
DEFS = -DLAIK_DEBUG
OPT = -g
SUBDIRS=examples

# include Makefile.config if existing
# this is generated by 'configure'
-include Makefile.config

CFLAGS=$(OPT) -std=gnu99 -Iinclude
LDFLAGS=$(OPT)

SRCS = $(wildcard src/*.c)
HEADERS = $(wildcard include/*.h include/laik/*.h)
OBJS = $(SRCS:.c=.o)
DEPS = $(SRCS:.c=.d)

# instruct GCC to produce dependency files
CFLAGS+=-MMD -MP
CFLAGS+=$(DEFS)

# build targets
.PHONY: $(SUBDIRS)

all: liblaik.a $(SUBDIRS)

liblaik.a: $(OBJS)
	ar rcs liblaik.a $(OBJS)

examples: liblaik.a
	cd examples && $(MAKE) CC=$(CC) DEFS='$(DEFS)'

external/MQTT: liblaik.a
	cd external/MQTT && $(MAKE) CC=$(CC) CFLAGS='$(DEFS)'

# tests
test: examples
	echo "Dummy test is working!"

# clean targets
SUBDIRS_CLEAN=$(addprefix clean_, $(SUBDIRS))
.PHONY: $(SUBDIRS_CLEAN)

clean: clean_laik $(SUBDIRS_CLEAN)

clean_laik:
	rm -f *~ *.o $(OBJS) $(DEPS) liblaik.a

$(SUBDIRS_CLEAN): clean_%:
	+$(MAKE) clean -C $*


# install targets
install: install_laik

install_laik: liblaik.a $(HEADERS)
	cp $(wildcard include/*.h) $(PREFIX)/include
	mkdir -p $(PREFIX)/include/laik
	cp $(wildcard include/laik/*.h) $(PREFIX)/include/laik
	mkdir -p $(PREFIX)/lib
	cp liblaik.a $(PREFIX)/lib

# install targets
uninstall: uninstall_laik

uninstall_laik:
	rm -rf $(PREFIX)/include/laik
	rm -f $(PREFIX)/include/laik.h
	rm -f $(PREFIX)/include/laik-internal.h
	rm -f $(PREFIX)/include/laik-backend-*.h
	rm -f $(PREFIX)/lib/liblaik.a

# include previously generated dependency rules if existing
-include $(DEPS)
