#include <string.h>

#include <molerat/tasklet.h>
#include <molerat/application.h>
#include <molerat/delim_stream.h>

#include "stream_utils.h"

struct write_test {
	struct mutex mutex;
	struct tasklet tasklet;
	struct error err;

	struct delim_write *dw;
	int step;

	struct stream *stream;
	size_t pos;
};

static enum stream_result write_some(struct write_test *t,
				     const char *buf, size_t len)
{
	while (t->pos < len) {
		ssize_t res = stream_write(t->stream,
					   buf + t->pos, len - t->pos,
					   &t->tasklet, &t->err);
		if (res < 0)
			return res;
		else
			t->pos += res;
	}

	return STREAM_OK;
}

#define STEP t->step =__LINE__; case __LINE__

static void write_test(void *v_t)
{
	struct write_test *t = v_t;
	enum stream_result sres;

	switch (t->step) {
	case 0:
		t->stream = delim_write_next(t->dw);
		t->pos = 0;

	STEP:
		sres = write_some(t, "hello, ", 7);
		if (sres == STREAM_WAITING)
			goto out;

		assert(sres == STREAM_OK);

	STEP:
		sres = stream_close(t->stream, &t->tasklet, &t->err);
		if (sres == STREAM_WAITING)
			goto out;

		assert(sres == STREAM_OK);
		stream_destroy(t->stream);
		t->stream = delim_write_next(t->dw);
		t->pos = 0;

	STEP:
		sres = write_some(t, "world!", 6);
		if (sres == STREAM_WAITING)
			goto out;

		assert(sres == STREAM_OK);

	STEP:
		sres = stream_close(t->stream, &t->tasklet, &t->err);
		if (sres == STREAM_WAITING)
			goto out;

		assert(sres == STREAM_OK);
		stream_destroy(t->stream);
	}

	tasklet_stop(&t->tasklet);
	application_stop();

 out:
	mutex_unlock(&t->mutex);
}

static void do_write_test(struct stream *stream)
{
	struct write_test t;

	mutex_init(&t.mutex);
	tasklet_init(&t.tasklet, &t.mutex, &t);
	error_init(&t.err);
	t.dw = delim_write_create(stream);
	t.step = 0;

	mutex_lock(&t.mutex);
	tasklet_goto(&t.tasklet, write_test);
	application_run();

	mutex_lock(&t.mutex);
	delim_write_destroy(t.dw);
	error_fini(&t.err);
	tasklet_fini(&t.tasklet);
	mutex_unlock_fini(&t.mutex);
}


struct read_test {
	struct mutex mutex;
	struct tasklet tasklet;
	struct error err;

	struct delim_read *dr;
	int step;

	struct stream *stream;
	size_t pos;
	char buf[10];
};

static ssize_t read_some(struct read_test *t, size_t len)
{
	for (;;) {
		ssize_t res = stream_read(t->stream,
					  t->buf + t->pos, len - t->pos,
					  &t->tasklet, &t->err);
		if (res == STREAM_END)
			return t->pos;
		else if (res < 0)
			return res;
		else
			t->pos += res;
	}
}

static void read_test(void *v_t)
{
	struct read_test *t = v_t;
	enum stream_result sres;
	ssize_t res;

	switch (t->step) {
	case 0:
		t->stream = delim_read_next(t->dr);
		t->pos = 0;

	STEP:
		res = read_some(t, 8);
		if (res == STREAM_WAITING)
			goto out;

		assert(res == 7);
		assert(!memcmp(t->buf, "hello, ", 7));

	STEP:
		sres = stream_close(t->stream, &t->tasklet, &t->err);
		if (sres == STREAM_WAITING)
			goto out;

		assert(sres == STREAM_OK);
		stream_destroy(t->stream);
		t->stream = delim_read_next(t->dr);
		t->pos = 0;

	STEP:
		res = read_some(t, 7);
		if (res == STREAM_WAITING)
			goto out;

		assert(res == 6);
		assert(!memcmp(t->buf, "world!", 6));

	STEP:
		sres = stream_close(t->stream, &t->tasklet, &t->err);
		if (sres == STREAM_WAITING)
			goto out;

		assert(sres == STREAM_OK);
		stream_destroy(t->stream);
	}

	tasklet_stop(&t->tasklet);
	application_stop();

 out:
	mutex_unlock(&t->mutex);
}

static void do_read_test(struct stream *stream)
{
	struct read_test t;

	mutex_init(&t.mutex);
	tasklet_init(&t.tasklet, &t.mutex, &t);
	error_init(&t.err);
	t.dr = delim_read_create(stream);
	t.step = 0;

	mutex_lock(&t.mutex);
	tasklet_goto(&t.tasklet, read_test);
	application_run();

	mutex_lock(&t.mutex);
	delim_read_destroy(t.dr);
	error_fini(&t.err);
	tasklet_fini(&t.tasklet);
	mutex_unlock_fini(&t.mutex);
}


int main(void)
{
	struct growbuf buf;

	application_prepare();

	growbuf_init(&buf, 100);
	do_write_test(growbuf_write_stream_create(&buf));
	do_read_test(bytes_read_stream_create(growbuf_to_bytes(&buf)));

	growbuf_reset(&buf);
	do_write_test(byte_at_a_time_stream_create(
					     growbuf_write_stream_create(&buf)));
	do_read_test(byte_at_a_time_stream_create(
			      bytes_read_stream_create(growbuf_to_bytes(&buf))));

	growbuf_fini(&buf);
	return 0;
}
