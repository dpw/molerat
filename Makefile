CFLAGS=-Wall -g -D_GNU_SOURCE

SRCS=base.c buffer.c buffer_test.c thread.c tasklet.c queue.c poll.c socket.c echo_server.c tasklet_test.c queue_test.c socket_test.c echo_server_main.c http-parser/http_parser.c http_reader.c http_server.c http_server_main.c http_client.c skinny-mutex/skinny_mutex.c
HDRS=base.h buffer.h thread.h tasklet.h queue.h poll.h socket.h echo_server.h http-parser/http_parser.h http_reader.h http_server.h skinny-mutex/skinny_mutex.h

$(foreach H,$(HDRS),$(eval HDROBJS_$(H)=$(H:%.h=%.o)))

TESTS=buffer_test tasklet_test queue_test socket_test
EXECUTABLES=$(TESTS) echo_server http_server http_client

MAINOBJ_echo_server=echo_server_main.o
MAINOBJ_http_server=http_server_main.o

.PHONY: all
all: $(EXECUTABLES)

clean_wildcards=$(1)*.o $(1)*.dep $(1)*~ $(1)*.gcov $(1)*.gcda $(1)*.gcno

.PHONY: clean
clean:
	rm -f $(call clean_wildcards) $(call clean_wildcards,http-parser/) $(call clean_wildcards,skinny-mutex/) $(EXECUTABLES)

%.o %.c.dep : %.c
	$(COMPILE.c) -MD -o $*.o $<
	@cat $*.d >$*.c.dep
	@sed -e 's/#.*//;s/^[^:]*://;s/ *\\$$//;s/^ *//;/^$$/d;s/$$/ :/' <$*.d >>$*.c.dep
	@sed -e 's/#.*//;s| /[^ ]*||g;s/ .*\.c//;/^ \\$$/d;s/^\([^ ][^ ]*\):/OBJNEEDS_\1=/;s/\([^ ]*\.h\)/\$$(HDROBJS_\1)/g' <$*.d >>$*.c.dep
	@rm $*.d

ifndef MAKECMDGOALS
TESTABLEGOALS:=$(.DEFAULT_GOAL)
else
TESTABLEGOALS:=$(MAKECMDGOALS)
endif

ifneq "$(strip $(patsubst clean%,,$(patsubst %clean,,$(TESTABLEGOALS))))" ""
-include $(SRCS:.c=.c.dep)
endif

objneeds_aux=$(if $(SAW_$(1)),,$(eval SAW_$(1):=1)$(eval SEEN+=$(1))$(foreach O,$(OBJNEEDS_$(1)),$(call objneeds_aux,$(O))))
objneeds=$(eval SEEN:=)$(call objneeds_aux,$(1))$(foreach O,$(SEEN),$(eval undefine SAW_$(O)))$(SEEN)$(eval undefine SEEN)

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
