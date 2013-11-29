#include <stdarg.h>

#include <molerat/http_writer.h>
#include <molerat/http_status.h>
#include <molerat/stream.h>

void http_writer_init(struct http_writer *w, struct stream *stream)
{
	w->state = HTTP_WRITER_INIT;
	w->stream = stream;
	growbuf_init(&w->prebody, 1000);
}

void http_writer_fini(struct http_writer *w)
{
	growbuf_fini(&w->prebody);
}

void http_writer_request(struct http_writer *w, const char *url)
{
	assert(w->state == HTTP_WRITER_INIT);
	growbuf_reset(&w->prebody);
	growbuf_printf(&w->prebody, "GET %s HTTP/1.1\r\n", url);
	w->state = HTTP_WRITER_HEADERS;
}

void http_writer_response(struct http_writer *w, int code)
{
	struct http_status *status = http_status_lookup(code);

	assert(w->state == HTTP_WRITER_INIT);
	if (!status)
		die("unknown HTTP status code %d", code);

	growbuf_reset(&w->prebody);
	growbuf_append(&w->prebody, status->message, status->message_len);
	w->state = HTTP_WRITER_HEADERS;
}

void http_writer_header(struct http_writer *w, const char *name,
			const char *val)
{
	assert(w->state == HTTP_WRITER_HEADERS);
	growbuf_append_string(&w->prebody, name);
	growbuf_append_string(&w->prebody, ": ");
	growbuf_append_string(&w->prebody, val);
	growbuf_append_string(&w->prebody, "\r\n");
}

void http_writer_headerf(struct http_writer *w, const char *name,
			 const char *fmt, ...)
{
	va_list ap;

	assert(w->state == HTTP_WRITER_HEADERS);

	va_start(ap, fmt);
	growbuf_append_string(&w->prebody, name);
	growbuf_append_string(&w->prebody, ": ");
	growbuf_vprintf(&w->prebody, fmt, ap);
	growbuf_append_string(&w->prebody, "\r\n");
	va_end(ap);
}

static ssize_t finish_prebody(struct http_writer *w,
			      struct tasklet *t, struct error *e)
{
	switch (w->state) {
	case HTTP_WRITER_HEADERS:
		growbuf_append_string(&w->prebody, "\r\n");
		w->prebody_out = growbuf_to_bytes(&w->prebody);
		w->state = HTTP_WRITER_PREBODY;
		/* fall through */

	case HTTP_WRITER_PREBODY: {
		size_t len = bytes_length(w->prebody_out);
		while (len) {
			ssize_t res = stream_write(w->stream,
					      bytes_current(w->prebody_out),
					      len, t, e);
			if (res < 0)
				return res;

			bytes_advance(&w->prebody_out, res);
			len -= res;
		}

		w->state = HTTP_WRITER_BODY;
		/* fall through */
	}

	case HTTP_WRITER_BODY:
		return 0;

	default:
		die("bad http_writer state %d", w->state);
	}
}

ssize_t http_writer_write(struct http_writer *w, const void *buf, size_t len,
			  struct tasklet *t, struct error *e)
{
	ssize_t res = finish_prebody(w, t, e);
	if (res >= 0)
		res = stream_write(w->stream, buf, len, t, e);

	return res;
}

enum http_writer_end_result http_writer_end(struct http_writer *w,
					    struct tasklet *t, struct error *e)
{
	ssize_t res = finish_prebody(w, t, e);
	switch (res) {
	case STREAM_WAITING:
		return HTTP_WRITER_END_WAITING;

	case STREAM_ERROR:
		return HTTP_WRITER_END_ERROR;

	case 0:
		w->state = HTTP_WRITER_INIT;
		return HTTP_WRITER_END_DONE;

	default:
		die("bad http_writer finish_prebody return %ld", (long)res);
	}
}
