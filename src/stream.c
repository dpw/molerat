#include <molerat/stream.h>
#include <molerat/buffer.h>

ssize_t stream_read_only_write(struct stream *gs, const void *buf,
			       size_t len, struct tasklet *t,
			       struct error *err)
{
	(void)gs;
	(void)buf;
	(void)len;
	(void)t;

	error_set(err, ERROR_INVALID, "write to a read-only stream");
	return STREAM_ERROR;
}

ssize_t stream_write_only_read(struct stream *gs, void *buf,
			       size_t len, struct tasklet *t,
			       struct error *err)
{
	(void)gs;
	(void)buf;
	(void)len;
	(void)t;

	error_set(err, ERROR_INVALID, "read from a write-only stream");
	return STREAM_ERROR;
}

enum stream_result stream_noop_close(struct stream *gs, struct tasklet *t,
				     struct error *err)
{
	(void)gs;
	(void)t;
	(void)err;

	return STREAM_OK;
}

ssize_t stream_read_growbuf(struct stream *s, struct growbuf *gb,
			    struct tasklet *t, struct error *err)
{
	ssize_t res;
	size_t space = growbuf_space(gb);

	if (!space) {
		/* Growbuf is full, so make some space */
		growbuf_grow(gb, 1);
		space = growbuf_space(gb);
	}

	res = stream_read(s, growbuf_end(gb), space, t, err);
	if (res > 0)
		growbuf_advance(gb, res);

	return res;
}

struct stream_pump {
	struct stream *source;
	struct stream *dest;
	char *pos;
	size_t len;
	size_t buf_size;
	char buf[1];
};

struct stream_pump *stream_pump_create(struct stream *source,
				       struct stream *dest,
				       size_t buf_size)
{
	struct stream_pump *sp = xalloc(sizeof *sp + buf_size - 1);
	sp->source = source;
	sp->dest = dest;
	sp->len = 0;
	sp->buf_size = buf_size;
	return sp;
}

void stream_pump_destroy(struct stream_pump *sp)
{
	free(sp);
}

void stream_pump_destroy_with_source(struct stream_pump *sp)
{
	stream_destroy(sp->source);
	free(sp);
}

void stream_pump_destroy_with_dest(struct stream_pump *sp)
{
	stream_destroy(sp->dest);
	free(sp);
}

void stream_pump_destroy_with_streams(struct stream_pump *sp)
{
	stream_destroy(sp->source);
	stream_destroy(sp->dest);
	free(sp);
}

ssize_t stream_pump(struct stream_pump *sp, struct tasklet *t,
		    struct error *err)
{
	ssize_t res;
	ssize_t total = 0;

	for (;;) {
		if (!sp->len) {
			res = stream_read(sp->source, sp->buf, sp->buf_size,
					  t, err);
			if (res < 0)
				break;
			sp->pos = sp->buf;
			sp->len = res;
			continue;
		}

		res = stream_write(sp->dest, sp->pos, sp->len, t, err);
		if (res < 0)
			break;

		sp->pos += res;
		sp->len -= res;
		total += res;
	}

	return res != STREAM_WAITING ? res : total;
}
