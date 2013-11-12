#include "http_writer.h"
#include "socket.h"

void http_writer_init(struct http_writer *w, struct socket *socket)
{
	w->state = HTTP_WRITER_INIT;
	w->socket = socket;
	growbuf_init(&w->prebody, 1000);
}

void http_writer_fini(struct http_writer *w)
{
	growbuf_fini(&w->prebody);
}

void http_writer_start_request(struct http_writer *w, const char *url)
{
	assert(w->state == HTTP_WRITER_INIT);
	growbuf_printf(&w->prebody, "GET %s HTTP/1.1\r\n", url);
	w->state = HTTP_WRITER_HEADERS;
}

void http_writer_write_header(struct http_writer *w, const char *name,
			      const char *val)
{
	assert(w->state == HTTP_WRITER_HEADERS);
	growbuf_append_string(&w->prebody, name);
	growbuf_append_string(&w->prebody, ": ");
	growbuf_append_string(&w->prebody, val);
	growbuf_append_string(&w->prebody, "\r\n");
}

static ssize_t finish_prebody(struct http_writer *w,
			      struct tasklet *t, struct error *e)
{
	switch (w->state) {
	case HTTP_WRITER_HEADERS:
		growbuf_append_string(&w->prebody, "\r\n");
		growbuf_to_drainbuf(&w->prebody, &w->prebody_out);
		w->state = HTTP_WRITER_PREBODY;
		/* fall through */

	case HTTP_WRITER_PREBODY: {
		size_t len = drainbuf_length(&w->prebody_out);
		while (len) {
			ssize_t res = socket_write(w->socket,
					      drainbuf_current(&w->prebody_out),
					      len, t, e);
			if (res < 0)
				return res;

			drainbuf_advance(&w->prebody_out, res);
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

ssize_t http_writer_write(struct http_writer *w, void *buf, size_t len,
			  struct tasklet *t, struct error *e)
{
	ssize_t res = finish_prebody(w, t, e);
	if (res >= 0)
		res = socket_write(w->socket, buf, len, t, e);

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
