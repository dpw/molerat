# Make version check
REQUIRED_MAKE_VERSION:=3.81
ifneq ($(shell ( echo "$(MAKE_VERSION)" ; echo "$(REQUIRED_MAKE_VERSION)" ) | sort -t. -n | head -1),$(REQUIRED_MAKE_VERSION))
$(error GNU make version $(REQUIRED_MAKE_VERSION) required)
endif

.DEFAULT_GOAL:=all

MAKEFILE:=$(firstword $(MAKEFILE_LIST))

# You can override this from the command line
CFLAGS=-Wall -Wextra -Werror -ansi -g

# ROOT is the path to the source tree.  If non-empty, then it includes
# a trailing slash.
ROOT=$(filter-out ./,$(dir $(MAKEFILE)))

# VPATH tells make where to search for sources, if buliding from a
# separate build tree.
VPATH=$(ROOT)

# It's less likely that you'll want to override this
PROJECT_CFLAGS=-D_GNU_SOURCE -I$(ROOT)include -Wpointer-arith

# Principal source files under src/
SRCS=base.c buffer.c thread.c tasklet.c application.c queue.c \
	poll_common.c poll_poll.c poll_epoll.c socket.c echo_server.c \
	echo_server_main.c http-parser/http_parser.c http_reader.c \
	http_status_gen.c http_writer.c http_server.c http_server_main.c \
	http_client.c http_status.c skinny-mutex/skinny_mutex.c \
	stream.c delim_stream.c

# Test source files under test/
TEST_SRCS=buffer_test.c tasklet_test.c queue_test.c socket_test.c timer_test.c \
	stream_utils.c http_reader_test.c delim_stream_test.c

# Header files under include
HDRS=include/http-parser/http_parser.h include/skinny-mutex/skinny_mutex.h \
	$(addprefix include/molerat/,base.h buffer.h thread.h tasklet.h \
		application.h queue.h watched_fd.h stream.h socket.h \
		echo_server.h http_reader.h http_status.h http_writer.h \
		http_server.h delim_stream.h) src/poll.h test/stream_utils.h

# Main exectuables that get built
EXECUTABLES=echo_server http_status_gen http_server http_client

# Test executables that get built
TEST_EXECUTABLES=buffer_test tasklet_test queue_test socket_test timer_test \
	http_reader_test delim_stream_test

# All source directories
SRCDIRS=src src/skinny-mutex src/http-parser test

# These HDROBJS definitions say which object files correspond to which
# headers.  I.e., if the header file is included, then the given object
# files should be linked in.
HDROBJS_$(ROOT)src/poll.h=src/poll_common.o
HDROBJS_$(ROOT)test/stream_utils.h=test/stream_utils.o

ifdef USE_EPOLL
HDROBJS_$(ROOT)include/molerat/watched_fd.h=src/poll_epoll.o
HDROBJS_$(ROOT)include/molerat/timer.h=src/poll_epoll.o
else
HDROBJS_$(ROOT)include/molerat/watched_fd.h=src/poll_poll.o
HDROBJS_$(ROOT)include/molerat/timer.h=src/poll_poll.o
endif

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
$(foreach H,$(HDRS),$(eval HDROBJS_$(ROOT)$(H)?=src/$(notdir $(H:%.h=%.o))))

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
	$$(CC) $$(CFLAGS) $(PROJECT_CFLAGS) $$(build_executable_objneeds_$(1)) -o $$@
else
$(1):
	@false
endif
endef

$(foreach E,$(EXECUTABLES),$(eval $(call build_executable,$(E),src/)))
$(foreach E,$(TEST_EXECUTABLES),$(eval $(call build_executable,$(E),test/)))

.PHONY: run_tests
run_tests: $(TEST_EXECUTABLES)
	$(foreach T,$(TEST_EXECUTABLES),./$(T) &&) :

.PHONY: coverage
coverage:
	$(MAKE) -f $(MAKEFILE) clean
	$(MAKE) -f $(MAKEFILE) all CFLAGS="$(CFLAGS) --coverage"
	$(foreach T,$(TEST_EXECUTABLES),./$(T) &&) :
	mkdir -p coverage
	gcov -p $(ALL_SRCS)
	mv *.gcov coverage
