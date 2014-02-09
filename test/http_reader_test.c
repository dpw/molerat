#include <string.h>

#include <molerat/tasklet.h>
#include <molerat/application.h>
#include <molerat/http_reader.h>

#include "stream_utils.h"

static int compare_growbufs(const void *a, const void *b)
{
	int res;
	size_t len = growbuf_length(a);
	if (growbuf_length(b) < len)
		len = growbuf_length(b);

	res = memcmp(growbuf_offset(a, 0), growbuf_offset(b, 0), len);
	if (res)
		return res;

	return (growbuf_length(a) > len) - (growbuf_length(b) > len);
}

static void check_headers(struct http_reader *r, int count, const char *expect)
{
	struct http_header_iter iter;
	struct http_header *header;
	struct growbuf *bufs = xalloc(count * sizeof *bufs);
	int i;

	for (i = 0, http_reader_headers(r, &iter);
	     (header = http_header_iter_next(&iter));
	     i++) {
		assert(i < count);
		growbuf_init(&bufs[i], 100);
		growbuf_printf(&bufs[i], "<%.*s>=<%.*s>",
			       (int)header->name_len, header->name,
			       (int)header->value_len, header->value);
	}

	assert(i == count);
	qsort(bufs, count, sizeof(struct growbuf), compare_growbufs);

	for (i = 1; i < count; i++) {
		growbuf_append_string(&bufs[0], ",");
		growbuf_append_growbuf(&bufs[0], &bufs[i]);
	}

	assert(growbuf_length(&bufs[0]) == strlen(expect));
	assert(memcmp(growbuf_offset(&bufs[0], 0), expect,
		      growbuf_length(&bufs[0])) == 0);

	for (i = 0; i < count; i++)
		growbuf_fini(&bufs[i]);

	free(bufs);
}

static const char test_data[] =
	"GET /req1 HTTP/1.1\r\n"
	"Host: foo.example.com\r\n"
	"User-Agent: UA1\r\n"
	"\r\n"

	"POST /req2 HTTP/1.1\r\n"
	"Host: bar.example.com\r\n"
	"User-Agent: UA2\r\n"
	"Transfer-Encoding: chunked\r\n"
	"\r\n"
	"7\r\n"
	"hello, \r\n"
	"6\r\n"
	"world!\r\n"
	"0\r\n"
	"\r\n"

	"GET /req3 HTTP/1.1\r\n"
	"Host: baz.example.com\r\n"
	"User-Agent: UA3\r\n"
	"Continuated-Header: foo\r\n"
	" bar\r\n"
	"\r\n";

struct http_reader_test {
	struct mutex mutex;
	struct tasklet tasklet;
	struct error err;

	struct stream *stream;
	int step;

	struct http_reader reader;
	size_t pos;
	char buf[14];
};

static ssize_t read_body(struct http_reader_test *t, size_t len)
{
	for (;;) {
		ssize_t res = http_reader_body(&t->reader,
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

#define STEP t->step =__LINE__; case __LINE__

static void http_reader_test(void *v_t)
{
	struct http_reader_test *t = v_t;
	enum http_reader_prebody_result pres;
	ssize_t res;

	switch (t->step) {
	case 0:
		http_reader_init_request(&t->reader, t->stream);

	STEP:
		/* First request */
		pres = http_reader_prebody(&t->reader, &t->tasklet, &t->err);
		if (pres == HTTP_READER_PREBODY_WAITING
		    || pres == HTTP_READER_PREBODY_PROGRESS)
			return;

		assert(pres == HTTP_READER_PREBODY_DONE);
		assert(http_reader_method(&t->reader) == HTTP_GET);
		assert(bytes_compare(http_reader_url(&t->reader),
				     c_string_bytes("req1")));
		check_headers(&t->reader, 2,
			      "<Host>=<foo.example.com>,<User-Agent>=<UA1>");

	STEP:
		res = http_reader_body(&t->reader, t->buf, 1, &t->tasklet,
				       &t->err);
		if (res == STREAM_WAITING)
			return;

		assert(res == STREAM_END);

	STEP:
		/* Second request */
		pres = http_reader_prebody(&t->reader, &t->tasklet, &t->err);
		if (pres == HTTP_READER_PREBODY_WAITING
		    || pres == HTTP_READER_PREBODY_PROGRESS)
			return;

		assert(pres == HTTP_READER_PREBODY_DONE);
		assert(http_reader_method(&t->reader) == HTTP_POST);
		assert(bytes_compare(http_reader_url(&t->reader),
				     c_string_bytes("req2")));
		check_headers(&t->reader, 3,
			      "<Host>=<bar.example.com>,<Transfer-Encoding>=<chunked>,<User-Agent>=<UA2>");

	STEP:
		/* try a 0-length read */
		res = http_reader_body(&t->reader, t->buf, 0, &t->tasklet,
				       &t->err);
		if (res == STREAM_WAITING)
			return;

		assert(res == 0);

		t->pos = 0;
	STEP:
		res = read_body(t, 14);
		if (res == STREAM_WAITING)
			return;

		assert(res == 13);
		assert(!memcmp(t->buf, "hello, world!", 13));

	STEP:
		/* Third request */
		pres = http_reader_prebody(&t->reader, &t->tasklet, &t->err);
		if (pres == HTTP_READER_PREBODY_WAITING
		    || pres == HTTP_READER_PREBODY_PROGRESS)
			return;

		assert(pres == HTTP_READER_PREBODY_DONE);
		assert(http_reader_method(&t->reader) == HTTP_GET);
		assert(bytes_compare(http_reader_url(&t->reader),
				     c_string_bytes("req3")));
		/* This is actually wrong, it should be "foo bar", but
		   looks like a bug in http-parser */
		check_headers(&t->reader, 3, "<Continuated-Header>=<foobar>,<Host>=<baz.example.com>,<User-Agent>=<UA3>");

	STEP:
		res = http_reader_body(&t->reader, t->buf, 1, &t->tasklet,
				       &t->err);
		if (res == STREAM_WAITING)
			return;

		assert(res == STREAM_END);

	STEP:
		/* End of stream */
		pres = http_reader_prebody(&t->reader, &t->tasklet, &t->err);
		if (pres == HTTP_READER_PREBODY_WAITING
		    || pres == HTTP_READER_PREBODY_PROGRESS)
			return;

		assert(pres == HTTP_READER_PREBODY_CLOSED);
	}

	http_reader_fini(&t->reader);

	tasklet_stop(&t->tasklet);
	application_stop();
}

static void do_http_reader_test(struct stream *stream)
{
	struct http_reader_test t;

	mutex_init(&t.mutex);
	tasklet_init(&t.tasklet, &t.mutex, &t);
	error_init(&t.err);
	t.stream = stream;
	t.step = 0;

	mutex_lock(&t.mutex);
	tasklet_goto(&t.tasklet, http_reader_test);

	mutex_unlock(&t.mutex);
	application_run();
	mutex_lock(&t.mutex);

	error_fini(&t.err);
	tasklet_fini(&t.tasklet);
	mutex_unlock_fini(&t.mutex);

	stream_destroy(stream);
}

int main(void)
{
	application_prepare_test();

	do_http_reader_test(bytes_read_stream_create(c_string_bytes(test_data)));
	do_http_reader_test(byte_at_a_time_stream_create(
			  bytes_read_stream_create(c_string_bytes(test_data))));

	return 0;
}
