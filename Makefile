# Make version check
REQUIRED_MAKE_VERSION:=3.81
ifneq ($(shell ( echo "$(MAKE_VERSION)" ; echo "$(REQUIRED_MAKE_VERSION)" ) | sort -t. -n | head -1),$(REQUIRED_MAKE_VERSION))
$(error GNU make version $(REQUIRED_MAKE_VERSION) required)
endif

# Preliminaries
.DEFAULT_GOAL:=all
MAKEFILE:=$(firstword $(MAKEFILE_LIST))
TARGET_OS:=$(shell uname -s)

# You can override this from the command line
CFLAGS=-Wall -Wextra -Werror -ansi -g

# clang doesn't like the use of '-ansi' when linking
LD_CFLAGS=$(filter-out -ansi,$(CFLAGS))

# ROOT is the path to the source tree.  If non-empty, then it includes
# a trailing slash.  By default, use the directory containing this
# Makefile.
ROOT=$(filter-out ./,$(dir $(MAKEFILE)))

# VPATH tells make where to search for sources, if buliding from a
# separate build tree.
VPATH=$(ROOT)

# It's less likely that you'll want to override this
PROJECT_CFLAGS:=-Wpointer-arith -I$(ROOT)include

# Principal source files under src/
SRCS=base.c buffer.c thread.c tasklet.c application.c queue.c \
	poll_common.c poll_poll.c socket.c echo_server.c \
	echo_server_main.c http-parser/http_parser.c http_reader.c \
	http_status_gen.c http_writer.c http_server.c http_server_main.c \
	http_client.c http_status.c skinny-mutex/skinny_mutex.c \
	stream.c delim_stream.c socket_transport.c
ifeq "$(TARGET_OS)" "Linux"
PROJECT_CFLAGS+=-D_GNU_SOURCE -I$(ROOT)include-linux
SRCS+=poll_epoll.c
USE_EPOLL=yes
endif
ifeq "$(TARGET_OS)" "FreeBSD"
PROJECT_CFLAGS+=-I$(ROOT)include-bsd
SRCS+=bsd/sort.c
HDROBJS_$(ROOT)include-bsd/molerat/sort.h=src/bsd/sort.o
endif

# Test source files under test/
TEST_SRCS=buffer_test.c tasklet_test.c queue_test.c socket_test.c timer_test.c \
	stream_utils.c http_reader_test.c delim_stream_test.c transport_test.c

# Main executables that get built
EXECUTABLES=echo_server http_status_gen http_server http_client

# Test executables that get built
TEST_EXECUTABLES=buffer_test tasklet_test queue_test socket_test timer_test \
	http_reader_test delim_stream_test transport_test

# All source directories
SRCDIRS=src src/skinny-mutex src/http-parser test

# These HDROBJS definitions say which object files correspond to which
# headers.  I.e., if the header file is included, then the given object
# files should be linked in.

# Most headers under include/molerat/ correspond to .c files under src/
$(foreach H,base.h buffer.h thread.h tasklet.h \
	application.h queue.h watched_fd.h stream.h socket.h \
	echo_server.h http_reader.h http_status.h http_writer.h \
	http_server.h delim_stream.h endian.h transport.h \
	socket_transport.h,\
	$(eval HDROBJS_$(ROOT)include/molerat/$(H)=src/$(notdir $(H:%.h=%.o))))

HDROBJS_$(ROOT)src/poll.h=src/poll_common.o
HDROBJS_$(ROOT)test/stream_utils.h=test/stream_utils.o

ifdef USE_EPOLL
HDROBJS_$(ROOT)include/molerat/watched_fd.h=src/poll_epoll.o
HDROBJS_$(ROOT)include/molerat/timer.h=src/poll_epoll.o
else
HDROBJS_$(ROOT)include/molerat/watched_fd.h=src/poll_poll.o
HDROBJS_$(ROOT)include/molerat/timer.h=src/poll_poll.o
endif

HDROBJS_$(ROOT)include/molerat/endian.h=
HDROBJS_$(ROOT)include/molerat/transport.h=
HDROBJS_$(ROOT)include/http-parser/http_parser.h=src/http-parser/http_parser.o
HDROBJS_$(ROOT)include/skinny-mutex/skinny_mutex.h=src/skinny-mutex/skinny_mutex.o

# This MAINOBJ definitions say which is the main object file of the
# given exectuable, in cases where the names don't match directly.
MAINOBJ_echo_server=src/echo_server_main.o
MAINOBJ_http_server=src/http_server_main.o

# That completes the definition of the project sources and structure.
# Now for the magic.

HDROBJS_/usr/include/pthread.h=-pthread
OBJNEEDS_-pthread=

ALL_EXECUTABLES:=$(EXECUTABLES) $(TEST_EXECUTABLES)
ALL_SRCS:=$(addprefix src/,$(SRCS)) $(addprefix test/,$(TEST_SRCS))

# Disable builtin rules
.SUFFIXES:

# Delete files produced by recipes that fail
.DELETE_ON_ERROR:

.PHONY: all
all: $(ALL_EXECUTABLES)

ifndef MAKECMDGOALS
TESTABLEGOALS:=$(.DEFAULT_GOAL)
else
TESTABLEGOALS:=$(MAKECMDGOALS)
endif

ifneq "$(strip $(patsubst clean%,,$(patsubst %clean,,$(TESTABLEGOALS))))" ""
-include $(foreach S,$(ALL_SRCS),$(S).dep)
endif

.PHONY: clean
clean::
	rm -f $(foreach D,$(SRCDIRS),$(D)/*.o $(D)/*.dep $(D)/*~ $(D)/*.gcda $(D)/*.gcno) $(ALL_EXECUTABLES) src/http_status.c coverage/*.gcov

src/http_status.c: http_status_gen
	$(<D)/$< >$@

%.o %.c.dep: %.c
	@mkdir -p $(@D)
	$(COMPILE.c) $(PROJECT_CFLAGS) -MD -o $*.o $<
	@sed -e 's|^\([^:]*\):|$*.o $*.c.dep:|' <$*.d >>$*.c.dep
	@sed -e 's/#.*//;s/^[^:]*://;s/ *\\$$//;s/^ *//;/^$$/d;s/$$/ :/' <$*.d >>$*.c.dep
	@sed -e 's/#.*//;s/ [^ ]*\.c//g;s/^\([^ ][^ ]*\):/OBJNEEDS_\1=/;s/\([^ ]*\.h\)/\$$(HDROBJS_\1)/g' <$*.d >>$*.c.dep
	@rm $*.d

# objneeds works out which object files are required to link the given
# object file.
objneeds=$(eval SEEN:=)$(call objneeds_aux,$(1))$(foreach O,$(SEEN),$(eval SAW_$(O):=))$(SEEN)

# objneeds_aux finds the transitive closure of the OBJNEEDS relation,
# starting with $(1), and putting the result in $(SEEN)
objneeds_aux=$(if $(SAW_$(1)),,$(eval SAW_$(1):=1)$(eval SEEN+=$(1))$(if $(filter-out -l%,$(1)),$(foreach O,$(call lookup_undefined,OBJNEEDS_$(1)),$(call objneeds_aux,$(O))),))

# Lookup the object files required to link $(1), returning 'undefined' if it was not defined.
lookup_undefined=$(if $(filter-out undefined,$(flavor $(1))),$($(1)),undefined)

define build_executable
build_executable_objneeds_$(1):=$(call objneeds,$(or $(MAINOBJ_$(1)),$(2)$(1).o))
ifeq "$$(filter undefined,$$(build_executable_objneeds_$(1)))" ""
$(1): $$(filter-out -pthread,$$(build_executable_objneeds_$(1)))
	$$(CC) $$(LD_CFLAGS) $(PROJECT_CFLAGS) $$(build_executable_objneeds_$(1)) -o $$@
else
$(1):
	@false
endif
endef

$(foreach E,$(EXECUTABLES),$(eval $(call build_executable,$(E),src/)))
$(foreach E,$(TEST_EXECUTABLES),$(eval $(call build_executable,$(E),test/)))

# This trivial variable can be called to produce a recipe line in
# contexts where that would otherwise be difficult, e.g. in a foreach
# function.
define recipe_line
$(1)

endef

.PHONY: run_tests
run_tests: $(TEST_EXECUTABLES)
	$(foreach T,$(TEST_EXECUTABLES),$(call recipe_line,./$(T)))

.PHONY: coverage
coverage:
	$(MAKE) -f $(MAKEFILE) clean
	$(MAKE) -f $(MAKEFILE) all CFLAGS="$(CFLAGS) --coverage"
	$(foreach T,$(TEST_EXECUTABLES),./$(T) &&) :
	mkdir -p coverage
	gcov -p $(ALL_SRCS)
	mv *.gcov coverage
