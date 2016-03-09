ifndef DEPTH

# Make version check
REQUIRED_MAKE_VERSION:=3.81
ifneq ($(shell ( echo "$(MAKE_VERSION)" ; echo "$(REQUIRED_MAKE_VERSION)" ) | sort -t. -n | head -1),$(REQUIRED_MAKE_VERSION))
$(error GNU make version $(REQUIRED_MAKE_VERSION) required)
endif

# Preliminaries
.DEFAULT_GOAL:=all
MAKEFILE:=$(lastword $(MAKEFILE_LIST))
TARGET_OS:=$(shell uname -s)

# VPATH tells make where to search for sources, if buliding from a
# separate build tree.
VPATH:=$(filter-out ./,$(dir $(MAKEFILE)))

ROOT:=

# You can override this from the command line
CFLAGS:=-g

# It's less likely that you'll want to override this
BASE_CFLAGS:=-Wpointer-arith -Wall -Wextra -Werror -ansi

ALL_SRCS:=
ALL_EXECUTABLES:=
ALL_TEST_EXECUTABLES:=
ALL_TO_CLEAN:=

# Disable builtin rules
.SUFFIXES:

# Delete files produced by recipes that fail
.DELETE_ON_ERROR:

.PHONY: clean

# Include a subproject
define include_subproject
ROOT:=$(1)/
include $(MAKEFILE)
endef

endif # not DEPTH


# This section is included once per project.  ROOT is set to the
# project source directory.

SUBPROJECTS:=
SRCS:=
EXECUTABLES:=
TEST_EXECUTABLES:=
TO_CLEAN:=
PROJECT_CFLAGS:=
SROOT:=$(VPATH)$(ROOT)
include $(SROOT)project.mk
ALL_SRCS+=$(addprefix $(ROOT),$(SRCS))
ALL_EXECUTABLES+=$(addprefix $(ROOT),$(EXECUTABLES))
ALL_TEST_EXECUTABLES+=$(addprefix $(ROOT),$(TEST_EXECUTABLES))
ALL_TO_CLEAN+=$(addprefix $(ROOT),$(TO_CLEAN))

ifdef PROJECT_CFLAGS
$(foreach S,$(SRCS),$(eval CFLAGS_$(ROOT)$(S):=$(BASE_CFLAGS) $(PROJECT_CFLAGS)))
endif

# Do subprojects
DEPTH:=$(DEPTH)x
$(foreach P,$(SUBPROJECTS),$(eval $(call include_subproject,$(ROOT)$(P))))
DEPTH:=$(patsubst %x,%,$(DEPTH))


ifndef DEPTH

.PHONY: all
all: $(ALL_EXECUTABLES) $(ALL_TEST_EXECUTABLES)

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
	rm -f $(foreach D,$(sort $(dir $(ALL_SRCS))),$(D)*.o $(D)*.dep $(D)*~ $(D)*.gcda $(D)*.gcno) $(strip $(ALL_EXECUTABLES) $(ALL_TEST_EXECUTABLES) $(ALL_TO_CLEAN))

%.o %.c.dep: %.c
	@mkdir -p $(@D)
	$(COMPILE.c) $(or $(CFLAGS_$(patsubst $(VPATH)%,%,$<)),$(BASE_CFLAGS)) $(and $(VPATH),-iquote$(patsubst $(VPATH)%,%,$(<D))) -MD -o $*.o $<
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
build_executable_objneeds_$(1):=$(call objneeds,$(MAINOBJ_$(1)))
ifeq "$$(filter undefined,$$(build_executable_objneeds_$(1)))" ""
$(1): $$(filter-out -pthread,$$(build_executable_objneeds_$(1)))
	$$(CC) $(filter-out -ansi,$(CFLAGS) $(BASE_CFLAGS)) $$(build_executable_objneeds_$(1)) -o $$@
else
$(1):
	@false
endif
endef

$(foreach E,$(ALL_EXECUTABLES) $(ALL_TEST_EXECUTABLES),$(eval $(call build_executable,$(E))))

# This trivial variable can be called to produce a recipe line in
# contexts where that would otherwise be difficult, e.g. in a foreach
# function.
define recipe_line
$(1)

endef

.PHONY: test
test: $(ALL_TEST_EXECUTABLES)
	$(foreach T,$(ALL_TEST_EXECUTABLES),$(call recipe_line,./$(T)))

.PHONY: coverage
coverage:
	$(MAKE) -f $(MAKEFILE) clean
	$(MAKE) -f $(MAKEFILE) all CFLAGS="$(CFLAGS) --coverage"
	$(foreach T,$(TEST_EXECUTABLES),./$(T) &&) :
	mkdir -p coverage
	gcov -p $(ALL_SRCS)
	mv *.gcov coverage

endif # DEPTH
