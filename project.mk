# Principal source files under src/
SRCS+=$(addprefix src/,base.c buffer.c thread.c tasklet.c application.c \
	queue.c poll_common.c poll_poll.c socket.c echo_server.c \
	echo_server_main.c http-parser/http_parser.c http_reader.c \
	http_status_gen.c http_writer.c http_server.c http_server_main.c \
	http_client.c http_status.c skinny-mutex/skinny_mutex.c \
	stream.c delim_stream.c socket_transport.c)

BASE_CFLAGS:=-I$(SROOT)include $(BASE_CFLAGS)

# OS specifics
ifeq "$(TARGET_OS)" "Linux"
BASE_CFLAGS:=-D_GNU_SOURCE -I$(SROOT)include-linux $(BASE_CFLAGS)
SRCS+=src/poll_epoll.c
HDROBJS_/usr/include/pthread.h:=-pthread
USE_EPOLL:=yes
endif
ifeq "$(TARGET_OS)" "FreeBSD"
BASE_CFLAGS:=-I$(SROOT)include-bsd $(BASE_CFLAGS)
SRCS+=src/bsd/sort.c
HDROBJS_$(SROOT)include-bsd/molerat/sort.h:=$(ROOT)src/bsd/sort.o
HDROBJS_/usr/include/pthread.h:=-pthread
endif
ifeq "$(TARGET_OS)" "Darwin"
BASE_CFLAGS:=-I$(SROOT)include-osx -I$(SROOT)include-bsd $(BASE_CFLAGS)
SRCS+=src/bsd/sort.c
HDROBJS_$(SROOT)include-bsd/molerat/sort.h:=$(ROOT)src/bsd/sort.o
HDROBJS_/usr/include/pthread.h:=
USE_PTHREAD_TLS:=yes
endif

ifdef USE_PTHREAD_TLS
BASE_CFLAGS+=-DUSE_PTHREAD_TLS
endif

# Test source files under test/
SRCS+=$(addprefix test/,buffer_test.c tasklet_test.c queue_test.c \
	socket_test.c timer_test.c stream_utils.c http_reader_test.c \
	delim_stream_test.c transport_test.c)

# Main executables that get built
EXECUTABLES+=echo_server http_status_gen http_server http_client

# Test executables that get built
TEST_EXECUTABLES+=buffer_test tasklet_test queue_test socket_test timer_test \
	http_reader_test delim_stream_test transport_test

# These HDROBJS definitions say which object files correspond to which
# headers.  I.e., if the header file is included, then the given object
# files should be linked in.

# Most headers under include/molerat/ correspond to .c files under src/
$(foreach H,base.h buffer.h thread.h tasklet.h \
	application.h queue.h watched_fd.h stream.h socket.h \
	echo_server.h http_reader.h http_status.h http_writer.h \
	http_server.h delim_stream.h endian.h transport.h \
	socket_transport.h,\
	$(eval HDROBJS_$(SROOT)include/molerat/$(H):=src/$(notdir $(H:%.h=%.o))))

HDROBJS_$(SROOT)src/poll.h:=$(ROOT)src/poll_common.o
HDROBJS_$(SROOT)test/stream_utils.h:=$(ROOT)test/stream_utils.o

ifdef USE_EPOLL
HDROBJS_$(SROOT)include/molerat/watched_fd.h:=$(ROOT)src/poll_epoll.o
HDROBJS_$(SROOT)include/molerat/timer.h:=$(ROOT)src/poll_epoll.o
else
HDROBJS_$(SROOT)include/molerat/watched_fd.h:=$(ROOT)src/poll_poll.o
HDROBJS_$(SROOT)include/molerat/timer.h:=$(ROOT)src/poll_poll.o
endif

HDROBJS_$(SROOT)include/molerat/endian.h:=
HDROBJS_$(SROOT)include/molerat/transport.h:=
HDROBJS_$(SROOT)include/http-parser/http_parser.h:=$(ROOT)src/http-parser/http_parser.o
HDROBJS_$(SROOT)include/skinny-mutex/skinny_mutex.h:=$(ROOT)src/skinny-mutex/skinny_mutex.o

# This MAINOBJ definitions say which is the main object file of the
# given exectuable, in cases where the names don't match directly.
$(foreach E,$(EXECUTABLES),$(eval MAINOBJ_$(ROOT)$(E):=$(ROOT)src/$(E).o))
$(foreach E,$(TEST_EXECUTABLES),$(eval MAINOBJ_$(ROOT)$(E):=$(ROOT)test/$(E).o))
MAINOBJ_$(ROOT)echo_server:=$(ROOT)src/echo_server_main.o
MAINOBJ_$(ROOT)http_server:=$(ROOT)src/http_server_main.o

OBJNEEDS_-pthread=

$(ROOT)src/http_status.c: $(ROOT)http_status_gen
	$(<D)/$< >$@

TO_CLEAN+=src/http_status.c
