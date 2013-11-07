# Make version check
REQUIRED_MAKE_VERSION:=3.81
ifneq ($(shell ( echo "$(MAKE_VERSION)" ; echo "$(REQUIRED_MAKE_VERSION)" ) | sort -t. -n | head -1),$(REQUIRED_MAKE_VERSION))
$(error GNU make version $(REQUIRED_MAKE_VERSION) required)
endif

CFLAGS=-Wall -Wextra -g -D_GNU_SOURCE

VPATH=$(SRCDIR)
SRCPATH=$(if $(SRCDIR),$(SRCDIR)/,)

SRCS=base.c buffer.c buffer_test.c thread.c tasklet.c application.c queue.c poll_common.c poll_poll.c poll_epoll.c socket.c echo_server.c tasklet_test.c queue_test.c socket_test.c echo_server_main.c http-parser/http_parser.c http_reader.c http_server.c http_server_main.c http_client.c skinny-mutex/skinny_mutex.c
HDRS=base.h buffer.h thread.h tasklet.h application.h queue.h poll.h watched_fd.h socket.h echo_server.h http-parser/http_parser.h http_reader.h http_server.h skinny-mutex/skinny_mutex.h

$(foreach H,$(HDRS),$(eval HDROBJS_$(SRCPATH)$(H)=$(H:%.h=%.o)))
HDROBJS_$(SRCPATH)poll.h=poll_common.o

ifdef USE_EPOLL
HDROBJS_$(SRCPATH)watched_fd.h=poll_epoll.o
else
HDROBJS_$(SRCPATH)watched_fd.h=poll_poll.o
endif

TESTS=buffer_test tasklet_test queue_test socket_test
EXECUTABLES=$(TESTS) echo_server http_server http_client

MAINOBJ_echo_server=echo_server_main.o
MAINOBJ_http_server=http_server_main.o

.PHONY: all
all: $(EXECUTABLES)

fileprefix=$(foreach F,$(2),$(if $(filter $(notdir $(F)),$(F)),$(1)$(F),$(dir $(F))$(1)$(notdir $(F))))
dotify=$(call fileprefix,.,$(1))
clean_wildcards=$(1)*.o $(call dotify,$(1)*.dep) $(1)*~ $(1)*.gcov $(1)*.gcda $(1)*.gcno

# Disable builtin rules
.SUFFIXES:

.PHONY: clean
clean:
	rm -f $(call clean_wildcards) $(call clean_wildcards,http-parser/) $(call clean_wildcards,skinny-mutex/) $(EXECUTABLES)

%.o $(call dotify,%.c.dep) : %.c
	@mkdir -p $(@D)
	$(COMPILE.c) -MD -o $*.o $<
	@cat $*.d >$(call dotify,$*.c.dep)
	@sed -e 's/#.*//;s/^[^:]*://;s/ *\\$$//;s/^ *//;/^$$/d;s/$$/ :/' <$*.d >>$(call dotify,$*.c.dep)
	@sed -e 's/#.*//;s| /[^ ]*||g;s/ .*\.c//;/^ \\$$/d;s/^\([^ ][^ ]*\):/OBJNEEDS_\1=/;s/\([^ ]*\.h\)/\$$(HDROBJS_\1)/g' <$*.d >>$(call dotify,$*.c.dep)
	@rm $*.d

ifndef MAKECMDGOALS
TESTABLEGOALS:=$(.DEFAULT_GOAL)
else
TESTABLEGOALS:=$(MAKECMDGOALS)
endif

ifneq "$(strip $(patsubst clean%,,$(patsubst %clean,,$(TESTABLEGOALS))))" ""
-include $(foreach SRC,$(SRCS),$(call dotify,$(SRC).dep))
endif

objneeds_aux=$(if $(SAW_$(1)),,$(eval SAW_$(1):=1)$(eval SEEN+=$(1))$(foreach O,$(OBJNEEDS_$(1)),$(call objneeds_aux,$(O))))
objneeds=$(eval SEEN:=)$(call objneeds_aux,$(1))$(foreach O,$(SEEN),$(eval SAW_$(O):=))$(SEEN)

define build_executable
$(1): $(call objneeds,$(or $(MAINOBJ_$(1)),$(1).o))
	$$(CC) $$(CFLAGS) $$^ -o $$@ -pthread
endef

$(foreach E,$(EXECUTABLES),$(eval $(call build_executable,$(E))))

.PHONY: test
test: $(TESTS)
	$(foreach T,$(TESTS),./$(T) &&) :

.PHONY: coverage
coverage:
	$(MAKE) clean
	$(MAKE) all CFLAGS="$(CFLAGS) --coverage"
	$(foreach T,$(TESTS),./$(T) &&) :
	$(foreach D,$(sort $(foreach S,$(SRCS),$(dir $(S)))),gcov -o $(D) $(strip $(foreach S,$(SRCS),$(and $(filter $(D),$(dir $(S))),$(S)))) &&) :
