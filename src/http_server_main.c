#include <stdio.h>
#include <string.h>

#include <molerat/thread.h>
#include <molerat/tasklet.h>
#include <molerat/socket.h>
#include <molerat/application.h>
#include <molerat/http_server.h>

struct exchange {
	struct http_server_exchange *server_exchange;
	struct http_reader *reader;
	struct http_writer *writer;

	struct mutex mutex;
	struct tasklet tasklet;
	struct error err;
	size_t body_len;
	size_t body_pos;
};

static void do_exchange(void *v_ex);
static void respond(struct exchange *ex);
static void write_body(void *v_ex);
static void finish_write(void *v_ex);

static void dump_headers(struct http_reader *r)
{
	struct http_header_iter iter;
	struct http_header *header;
	struct bytes url = http_reader_url(r);

	printf("URL <%.*s>\n", (int)bytes_length(url), bytes_current(url));

	for (http_reader_headers(r, &iter);
	     (header = http_header_iter_next(&iter));) {
		printf("Header <%.*s> <%.*s>\n",
		       (int)header->name_len, header->name,
		       (int)header->value_len, header->value);
	}
}

static void callback(struct http_server_exchange *sx,
		     struct http_reader *hr,
		     struct http_writer *hw)
{
	struct exchange *ex = xalloc(sizeof *ex);
	ex->server_exchange = sx;
	ex->reader = hr;
	ex->writer = hw;
	mutex_init(&ex->mutex);
	tasklet_init(&ex->tasklet, &ex->mutex, ex);
	error_init(&ex->err);

	dump_headers(hr);

	mutex_lock(&ex->mutex);
	tasklet_later(&ex->tasklet, do_exchange);
	mutex_unlock(&ex->mutex);
}

static void destroy_exchange_locked(struct exchange *ex)
{
	mutex_assert_held(&ex->mutex);
	error_fini(&ex->err);
	tasklet_fini(&ex->tasklet);
	mutex_unlock_fini(&ex->mutex);
	free(ex);
}

static void exchange_error(struct exchange *ex)
{
	fprintf(stderr, "Error: %s\n", error_message(&ex->err));
	http_server_exchange_done(ex->server_exchange, &ex->err);
	destroy_exchange_locked(ex);
}

static void do_exchange(void *v_ex)
{
	struct exchange *ex = v_ex;
	char buf[100];

	for (;;) {
		ssize_t res = http_reader_body(ex->reader, buf, 100,
					       &ex->tasklet, &ex->err);
		switch (res) {
		case STREAM_WAITING:
			return;

		case STREAM_ERROR:
			exchange_error(ex);
			return;

		case STREAM_END:
			respond(ex);
			return;

		default:
			assert(res >= 0);
			break;
		}

		fprintf(stderr, "Read %ld body bytes\n", (long)res);
	}
}

static const char body[] = "<html><body><h1>Hello from Molerat</h1><form action='/' method='post'><input type='hidden' name='foo' value='bar'><input type='submit' value='Send a POST request'></form></body></html>";

static void respond(struct exchange *ex)
{
	ex->body_len = strlen(body);
	ex->body_pos = 0;

	http_writer_response(ex->writer, 200);
	http_writer_header(ex->writer, "Server","Molerat");
	http_writer_headerf(ex->writer, "Content-Length", "%lu",
			    (unsigned long)ex->body_len);
	http_writer_header(ex->writer, "Content-Type",
			   "text/html; charset=utf-8");

	tasklet_goto(&ex->tasklet, write_body);
}


static void write_body(void *v_ex)
{
	struct exchange *ex = v_ex;

	while (ex->body_pos < ex->body_len) {
		ssize_t res = http_writer_write(ex->writer,
						body + ex->body_pos,
						ex->body_len - ex->body_pos,
						&ex->tasklet, &ex->err);
		switch (res) {
		case STREAM_WAITING:
			return;

		case STREAM_ERROR:
			exchange_error(ex);
			return;

		default:
			assert(res >= 0);
			break;
		}

		ex->body_pos += res;
	}

	tasklet_goto(&ex->tasklet, finish_write);
}

static void finish_write(void *v_ex)
{
	struct exchange *ex = v_ex;
	struct http_server_exchange *hse;

	switch (http_writer_end(ex->writer, &ex->tasklet, &ex->err)) {
	case HTTP_WRITER_END_WAITING:
		return;

	case HTTP_WRITER_END_ERROR:
		exchange_error(ex);
		return;

	case HTTP_WRITER_END_DONE:
		break;
	}

	hse = ex->server_exchange;
	destroy_exchange_locked(ex);
	http_server_exchange_done(hse, NULL);
}


int main(int argc, char **argv)
{
	struct error err;
	struct socket_factory *sf = socket_factory();
	struct server_socket *ss;
	struct http_server *hs = NULL;
	int res = 0;

	application_prepare();
	error_init(&err);

	switch (argc) {
	case 2:
		ss = socket_factory_bound_server_socket(sf, NULL, argv[1],
							&err);
		break;

	case 3:
		ss = socket_factory_bound_server_socket(sf, argv[1], argv[2],
							&err);
		break;

	default:
		error_set(&err, ERROR_MISC,
			  "usage: %s [<host>] <service>", argv[0]);
	}

	if (!error_ok(&err))
		goto out;

	hs = http_server_create(ss, callback);
	if (!hs)
		goto out;

	application_run();
	http_server_destroy(hs);

 out:
	res = !error_ok(&err);
	if (res) {
		fprintf(stderr, "%s\n", error_message(&err));
		res = 1;
	}

	error_fini(&err);
	return res;
}
