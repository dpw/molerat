#ifndef MOLERAT_STREAM_H
#define MOLERAT_STREAM_H

#include "base.h"

struct tasklet;

/* The outcome of reading from or writing to a stream is indicated by
 * a ssize_t.  Non-negative values mean progress was made (unlike for
 * read(2), zero result does not mean the stream was closed; it merely
 * means "try again").  Negative values are the following: */
enum {
	/* The operation could not be completed, and the tasklet has
	   been put on a wait_list to be woken when progress can be
	   made. */
	STREAM_WAITING = -1,

	/* An error occured, recorded in the struct error.*/
	STREAM_ERROR = -2,

	/* The end of the stream has been reached (only for reads). */
	STREAM_END = -3
};

struct stream {
	struct stream_ops *ops;
};

struct stream_ops {
	void (*destroy)(struct stream *s);
	ssize_t (*read)(struct stream *s, void *buf, size_t len,
			struct tasklet *t, struct error *e);
	ssize_t (*write)(struct stream *s, const void *buf, size_t len,
			 struct tasklet *t, struct error *e);
	void (*close)(struct stream *s, struct error *e);
};

static inline void stream_destroy(struct stream *s)
{
	return s->ops->destroy(s);
}

static inline ssize_t stream_read(struct stream *s, void *buf, size_t len,
				  struct tasklet *t, struct error *e)
{
	return s->ops->read(s, buf, len, t, e);
}

static inline ssize_t stream_write(struct stream *s, const void *buf,
				   size_t len, struct tasklet *t,
				   struct error *e)
{
	return s->ops->write(s, buf, len, t, e);
}

static inline void stream_close(struct stream *s, struct error *e)
{
	return s->ops->close(s, e);
}

#endif
