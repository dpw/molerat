#ifndef MOLERAT_STREAM_H
#define MOLERAT_STREAM_H

#include <molerat/base.h>

struct tasklet;

/* The outcome of reading from, writing to or closing a stream uses
 * the stream_result enum.  Reading and writing use a ssize_t where
 * non-negative values mean progress was made (unlike for read(2), a
 * zero result does not mean the stream was closed; it merely means
 * "try again"). */
enum stream_result {
	/* A close succeeded. */
	STREAM_OK = 0,

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
	enum stream_result (*close)(struct stream *s, struct tasklet *t,
				    struct error *e);
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

static inline enum stream_result stream_close(struct stream *s,
					      struct tasklet *t,
					      struct error *e)
{
	return s->ops->close(s, t, e);
}

ssize_t stream_read_only_write(struct stream *gs, const void *buf,
			       size_t len, struct tasklet *t,
			       struct error *err);
ssize_t stream_write_only_read(struct stream *gs, void *buf,
			       size_t len, struct tasklet *t,
			       struct error *err);
enum stream_result stream_noop_close(struct stream *gs,
				     struct tasklet *t,
				     struct error *err);

struct stream_pump;
struct stream_pump *stream_pump_create(struct stream *source,
				       struct stream *dest,
				       size_t buf_size);
void stream_pump_destroy(struct stream_pump *sp);
void stream_pump_destroy_with_source(struct stream_pump *sp);
void stream_pump_destroy_with_dest(struct stream_pump *sp);
void stream_pump_destroy_with_streams(struct stream_pump *sp);
ssize_t stream_pump(struct stream_pump *sp, struct tasklet *t,
		    struct error *err);

#endif
