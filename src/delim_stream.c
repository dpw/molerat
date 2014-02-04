#include <assert.h>

#include <molerat/delim_stream.h>

typedef uint16_t chunk_size_t;
#define MAX_CHUNK_SIZE 0x7fff
#define CHUNK_SIZE_END_BIT 0x8000

enum delim_state {
	DELIM_STATE_START = 0,
	/* DELIM_STATE_CUNK_INTRO_* values represent the number of
	   bytes written/read of the chunk intro */
	DELIM_STATE_CHUNK_INTRO_1,
	DELIM_STATE_PAYLOAD,
	/* DELIM_STATE_CUNK_FINAL_INTRO_* values represent the number of
	   bytes written of the closing chunk intro */
	DELIM_STATE_FINAL_CHUNK_INTRO_0,
	DELIM_STATE_FINAL_CHUNK_INTRO_1,
	DELIM_STATE_END
};

STATIC_ASSERT(DELIM_STATE_PAYLOAD == sizeof(chunk_size_t));
STATIC_ASSERT(DELIM_STATE_END
	      == DELIM_STATE_FINAL_CHUNK_INTRO_0 + sizeof(chunk_size_t));
#define DELIM_STATE_CHUNK_INTRO_CASES case DELIM_STATE_CHUNK_INTRO_1
#define DELIM_STATE_FINAL_CHUNK_INTRO_CASES \
	case DELIM_STATE_FINAL_CHUNK_INTRO_0: \
	case DELIM_STATE_FINAL_CHUNK_INTRO_1


struct delim_write_stream {
	struct stream base;
	struct delim_write *delim_write;
	struct stream *underlying;
	enum delim_state state;
	chunk_size_t chunk_left;
};

struct delim_write {
	struct stream *underlying;
	struct delim_write_stream *current;
};

struct delim_write *delim_write_create(struct stream *underlying)
{
	struct delim_write *dw = xalloc(sizeof *dw);
	dw->underlying = underlying;
	dw->current = NULL;
	return dw;
}

void delim_write_destroy(struct delim_write *dw)
{
	assert(!dw->current);
	stream_destroy(dw->underlying);
	free(dw);
}

static struct stream_ops delim_write_stream_ops;

struct stream *delim_write_next(struct delim_write *dw)
{
	struct delim_write_stream *s;

	assert(!dw->current);

	s = xalloc(sizeof *s);
	s->base.ops = &delim_write_stream_ops;
	s->delim_write = dw;
	s->underlying = dw->underlying;
	s->state = DELIM_STATE_START;

	dw->current = s;
	return &s->base;
}

static ssize_t write_payload(struct delim_write_stream *s, const void *buf,
			     size_t len, struct tasklet *t,
			     struct error *err)
{
	ssize_t res = stream_write(s->underlying, buf, len, t, err);

	if (res >= 0) {
		s->chunk_left -= res;
		if (!s->chunk_left)
			s->state = DELIM_STATE_START;
	}

	return res;
}

static ssize_t delim_write_stream_write(struct stream *gs, const void *buf,
					size_t len, struct tasklet *t,
					struct error *err)
{
	struct delim_write_stream *s
		= container_of(gs, struct delim_write_stream, base);
	ssize_t res;

	switch (s->state) {
	case DELIM_STATE_START:
		if (len > MAX_CHUNK_SIZE)
			len = MAX_CHUNK_SIZE;

		/* write the chunk intro */
		s->chunk_left = len;
		res = stream_write(s->underlying, &s->chunk_left,
				   sizeof(s->chunk_left), t, err);
		if (res <= 0)
			return res;

		s->state = res;
		if (unlikely((size_t)res < sizeof(s->chunk_left)))
			return 0;

		return write_payload(s, buf, len, t, err);

	DELIM_STATE_CHUNK_INTRO_CASES:
		res = stream_write(s->underlying,
				   (char *)&s->chunk_left + s->state,
				   sizeof(s->chunk_left) - s->state, t, err);
		if (res <= 0)
			return res;

		s->state += res;
		if (unlikely(s->state < sizeof(s->chunk_left)))
			return 0;

		/* fall through */

	case DELIM_STATE_PAYLOAD:
		if (len > s->chunk_left)
			len = s->chunk_left;

		return write_payload(s, buf, len, t, err);

	default:
		abort();
	}
}

static enum stream_result delim_write_stream_close(struct stream *gs,
						   struct tasklet *t,
						   struct error *err)
{
	struct delim_write_stream *s
		= container_of(gs, struct delim_write_stream, base);
	ssize_t res;

	switch (s->state) {
	case DELIM_STATE_START:
		s->chunk_left = CHUNK_SIZE_END_BIT;
		s->state = DELIM_STATE_FINAL_CHUNK_INTRO_0;

		do {
			res = stream_write(s->underlying, &s->chunk_left,
					   sizeof(s->chunk_left), t, err);
			if (res < 0)
				return res;

			s->state = DELIM_STATE_FINAL_CHUNK_INTRO_0 + res;
		} while (unlikely((size_t)res < sizeof(s->chunk_left)));

		s->delim_write->current = NULL;
		return STREAM_OK;

	DELIM_STATE_FINAL_CHUNK_INTRO_CASES:
		do {
			chunk_size_t pos
				= s->state - DELIM_STATE_FINAL_CHUNK_INTRO_0;
			chunk_size_t len = DELIM_STATE_END - s->state;
			res = stream_write(s->underlying,
					   (char *)&s->chunk_left + pos, len,
					   t, err);
			if (res < 0)
				return res;

			s->state += res;
		} while (unlikely(s->state < DELIM_STATE_END));

		s->delim_write->current = NULL;
		return STREAM_OK;

	default:
		abort();
	}
}

static void delim_write_stream_destroy(struct stream *gs)
{
	struct delim_write_stream *s
		= container_of(gs, struct delim_write_stream, base);

	assert(s->state == DELIM_STATE_END);
	free(s);
}

static struct stream_ops delim_write_stream_ops = {
	delim_write_stream_destroy,
	stream_write_only_read,
	delim_write_stream_write,
	delim_write_stream_close
};


struct delim_read_stream {
	struct stream base;
	struct delim_read *delim_read;
	struct stream *underlying;
	enum delim_state state;
	chunk_size_t chunk_left;
	bool_t last_chunk;
};

struct delim_read {
	struct stream *underlying;
	struct delim_read_stream *current;
};

struct delim_read *delim_read_create(struct stream *underlying)
{
	struct delim_read *dr = xalloc(sizeof *dr);
	dr->underlying = underlying;
	dr->current = NULL;
	return dr;
}

void delim_read_destroy(struct delim_read *dr)
{
	assert(!dr->current);
	stream_destroy(dr->underlying);
	free(dr);
}

static struct stream_ops delim_read_stream_ops;

struct stream *delim_read_next(struct delim_read *dr)
{
	struct delim_read_stream *s;

	assert(!dr->current);

	s = xalloc(sizeof *s);
	s->base.ops = &delim_read_stream_ops;
	s->delim_read = dr;
	s->underlying = dr->underlying;
	s->state = DELIM_STATE_START;
	s->last_chunk = FALSE;

	dr->current = s;
	return &s->base;
}

static ssize_t read_payload(struct delim_read_stream *s, void *buf,
			     size_t len, struct tasklet *t,
			     struct error *err)
{
	ssize_t res = stream_read(s->underlying, buf, len, t, err);

	if (res > 0) {
		s->chunk_left -= res;
		if (!s->chunk_left) {
			s->state = DELIM_STATE_START;
			if (s->last_chunk) {
				s->state = DELIM_STATE_END;
				s->delim_read->current = NULL;
			}
		}
	}

	return res;
}

static ssize_t delim_read_stream_read(struct stream *gs, void *buf,
				      size_t len, struct tasklet *t,
				      struct error *err)
{
	struct delim_read_stream *s
		= container_of(gs, struct delim_read_stream, base);
	ssize_t res;

	switch (s->state) {
	case DELIM_STATE_START:
		/* read the chunk intro */
		res = stream_read(s->underlying, &s->chunk_left,
				  sizeof(s->chunk_left), t, err);
		if (res <= 0)
			return res;

		s->state = res;
		if (unlikely((size_t)res < sizeof(s->chunk_left)))
			return 0;

	have_chunk_left:
		if (s->chunk_left & CHUNK_SIZE_END_BIT) {
			s->chunk_left &= ~CHUNK_SIZE_END_BIT;
			if (!s->chunk_left) {
				s->state = DELIM_STATE_END;
				s->delim_read->current = NULL;
				return STREAM_END;
			}
			else {
				s->last_chunk = TRUE;
			}
		}

		/* fall through */
	case DELIM_STATE_PAYLOAD:
		if (len > s->chunk_left)
			len = s->chunk_left;

		return read_payload(s, buf, len, t, err);

	DELIM_STATE_CHUNK_INTRO_CASES:
		res = stream_read(s->underlying,
				  (char *)&s->chunk_left + s->state,
				  sizeof(s->chunk_left) - s->state, t, err);
		if (res <= 0)
			return res;

		s->state += res;
		if (unlikely(s->state < sizeof(s->chunk_left)))
			return 0;

		goto have_chunk_left;

	case DELIM_STATE_END:
		return STREAM_END;

	default:
		abort();
	}
}

static enum stream_result delim_read_stream_close(struct stream *gs,
						  struct tasklet *t,
						  struct error *err)
{
	struct delim_read_stream *s
		= container_of(gs, struct delim_read_stream, base);

	(void)t;
	(void)err;

	assert(s->state == DELIM_STATE_END);
	return STREAM_OK;
}

static void delim_read_stream_destroy(struct stream *gs)
{
	struct delim_write_stream *s
		= container_of(gs, struct delim_write_stream, base);

	assert(s->state == DELIM_STATE_END);
	free(s);
}

static struct stream_ops delim_read_stream_ops = {
	delim_read_stream_destroy,
	delim_read_stream_read,
	stream_read_only_write,
	delim_read_stream_close
};
