#ifndef MOLERAT_STREAM_UTILS_H
#define MOLERAT_STREAM_UTILS_H

#include <molerat/stream.h>

struct drainbuf;

/* These streams don't do locking and so are just for use in tests. */

struct stream *drainbuf_read_stream_create(struct drainbuf *buf);

/* This stream reads and writes from the underlying stream a byte at a
   time. */
struct stream *byte_at_a_time_stream_create(struct stream *underlying);

#endif
