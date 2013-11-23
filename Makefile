# Make version check
REQUIRED_MAKE_VERSION:=3.81
ifneq ($(shell ( echo "$(MAKE_VERSION)" ; echo "$(REQUIRED_MAKE_VERSION)" ) | sort -t. -n | head -1),$(REQUIRED_MAKE_VERSION))
$(error GNU make version $(REQUIRED_MAKE_VERSION) required)
endif

MAKEFILE:=$(firstword $(MAKEFILE_LIST))

# You can override this from the command line
CFLAGS=-Wall -Wextra -ansi -g

# ROOT is the path to the source tree.  If non-empty, then it includes
# a trailing slash.
ROOT=$(filter-out ./,$(dir $(MAKEFILE)))

# VPATH tells make where to search for sources, if buliding from a
# separate build tree.
VPATH=$(ROOT)

# It's less likely that you'll want to override this
PROJECT_CFLAGS=-D_GNU_SOURCE -I$(ROOT)include

# Principal source files under src/
SRCS=base.c buffer.c thread.c tasklet.c application.c queue.c \
	poll_common.c poll_poll.c poll_epoll.c socket.c echo_server.c \
	echo_server_main.c http-parser/http_parser.c http_reader.c \
	http_status_gen.c http_writer.c http_server.c http_server_main.c \
	http_client.c skinny-mutex/skinny_mutex.c

# Test source files under test/
TEST_SRCS=buffer_test.c tasklet_test.c queue_test.c socket_test.c timer_test.c \
	stream_utils.c

# Header files under include
HDRS=include/http-parser/http_parser.h include/skinny-mutex/skinny_mutex.h \
	$(addprefix include/molerat/,base.h buffer.h thread.h tasklet.h \
		application.h queue.h watched_fd.h stream.h socket.h \
		echo_server.h http_reader.h http_status.h http_writer.h \
		http_server.h) src/poll.h test/stream_utils.h

# Main exectuables that get built
EXECUTABLES=echo_server http_status_gen http_server http_client

# Test executables that get built
TEST_EXECUTABLES=buffer_test tasklet_test queue_test socket_test timer_test

# All source directories
SRCDIRS=src src/skinny-mutex src/http-parser test

# These HDROBJS definitions say which object files correspond to which
# headers.  I.e., if the header file is included, then the give object
# files should be linked in.
HDROBJS_$(ROOT)include/molerat/stream.h=
HDROBJS_$(ROOT)src/poll.h=src/poll_common.o

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

ALL_EXECUTABLES:=$(EXECUTABLES) $(TEST_EXECUTABLES)
ALL_SRCS:=$(addprefix src/,$(SRCS)) $(addprefix test/,$(TEST_SRCS))
$(foreach H,$(HDRS),$(eval HDROBJS_$(ROOT)$(H)?=src/$(notdir $(H:%.h=%.o))))

# Disable builtin rules
.SUFFIXES:

.PHONY: all
all: $(ALL_EXECUTABLES)

ifndef MAKECMDGOALS
TESTABLEGOALS:=$(.DEFAULT_GOAL)
else
TESTABLEGOALS:=$(MAKECMDGOALS)
endif

# dotify puts a dot in front of the given filename, respecting any
# directories that may be in the path.
dotify=$(call fileprefix,.,$(1))
fileprefix=$(foreach F,$(2),$(if $(filter $(notdir $(F)),$(F)),$(1)$(F),$(dir $(F))$(1)$(notdir $(F))))

ifneq "$(strip $(patsubst clean%,,$(patsubst %clean,,$(TESTABLEGOALS))))" ""
-include $(foreach S,$(ALL_SRCS),$(call dotify,$(S).dep))
endif

.PHONY: clean
clean::
	rm -f $(foreach D,$(SRCDIRS),$(D)/*.o $(call dotify,$(D)/*.dep) $(D)/*~ $(D)/*.gcov $(D)/*.gcda $(D)/*.gcno) $(ALL_EXECUTABLES) src/http_status.c

src/http_status.c: http_status_gen
	$(<D)/$< >$@ || rm -f $@

ifneq ($(ROOT),)
root_frob=;s|$(ROOT)|\#|g
root_unfrob=;s|\#|$(ROOT)|g
endif

%.o $(call dotify,%.c.dep) : %.c
	@mkdir -p $(@D)
	$(COMPILE.c) $(PROJECT_CFLAGS) -MD -o $*.o $<
	@cat $*.d >$(call dotify,$*.c.dep)
	@sed -e 's/#.*//;s/^[^:]*://;s/ *\\$$//;s/^ *//;/^$$/d;s/$$/ :/' <$*.d >>$(call dotify,$*.c.dep)
	@sed -e 's/#.*//;s/ [^ ]*\.c//$(root_frob);s| /[^ ]*||g;/^ *\\$$/d$(root_unfrob);s/^\([^ ][^ ]*\):/OBJNEEDS_\1=/;s/\([^ ]*\.h\)/\$$(HDROBJS_\1)/g' <$*.d >>$(call dotify,$*.c.dep)
	@rm $*.d

# objneeds works out which object files are required to link the given
# object file.
objneeds=$(eval SEEN:=)$(call objneeds_aux,$(1))$(foreach O,$(SEEN),$(eval SAW_$(O):=))$(SEEN)
objneeds_aux=$(if $(SAW_$(1)),,$(eval SAW_$(1):=1)$(eval SEEN+=$(1))$(foreach O,$(OBJNEEDS_$(1)),$(call objneeds_aux,$(O))))

define build_executable
$(1): $(call objneeds,$(or $(MAINOBJ_$(1)),$(2)$(1).o))
	$$(CC) $$(CFLAGS) $(PROJECT_CFLAGS) $$^ -o $$@ -pthread
endef

$(foreach E,$(EXECUTABLES),$(eval $(call build_executable,$(E),src/)))
$(foreach E,$(TEST_EXECUTABLES),$(eval $(call build_executable,$(E),test/)))

.PHONY: test
test: $(TEST_EXECUTABLES)
	$(foreach T,$(TEST_EXECUTABLES),./$(T) &&) :

.PHONY: coverage
coverage:
	$(MAKE) -f $(MAKEFILE) clean
	$(MAKE) -f $(MAKEFILE) all CFLAGS="$(CFLAGS) --coverage"
	$(foreach T,$(TEST_EXECUTABLES),./$(T) &&) :
	$(foreach D,$(sort $(foreach S,$(ALL_SRCS),$(dir $(S)))),(cd $(D) && gcov $(strip $(foreach S,$(ALL_SRCS),$(and $(filter $(D),$(dir $(S))),$(notdir $(S)))))) &&) :
