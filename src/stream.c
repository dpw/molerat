#include <molerat/stream.h>

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
