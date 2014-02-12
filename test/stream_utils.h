#ifndef MOLERAT_STREAM_UTILS_H
#define MOLERAT_STREAM_UTILS_H

#include <molerat/stream.h>
#include <molerat/buffer.h>

/* These streams don't do locking and so are just for use in tests
   (although it is undecided whether streams have to do locking
   anyway). */

struct stream *bytes_read_stream_create(struct bytes buf);

static inline struct stream *c_string_read_stream_create(const char *s)
{
	return bytes_read_stream_create(c_string_bytes(s));
}

struct stream *growbuf_write_stream_create(struct growbuf *growbuf);

/* This stream reads and writes from the underlying stream a byte at a
   time. */
struct stream *byte_at_a_time_stream_create(struct stream *underlying);

#endif
