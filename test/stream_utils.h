#ifndef MOLERAT_STREAM_UTILS_H
#define MOLERAT_STREAM_UTILS_H

#include <molerat/stream.h>

struct bytes;

/* These streams don't do locking and so are just for use in tests. */

struct stream *bytes_read_stream_create(struct bytes buf);

/* This stream reads and writes from the underlying stream a byte at a
   time. */
struct stream *byte_at_a_time_stream_create(struct stream *underlying);

#endif
