#ifndef MOLERAT_STREAM_UTILS_H
#define MOLERAT_STREAM_UTILS_H

#include <molerat/stream.h>

struct drainbuf;

/* These streams don't do locking and so are just for use in tests. */

struct stream *drainbuf_read_stream_create(struct drainbuf *buf);

#endif
