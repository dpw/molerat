#include <stdio.h>

#include "socket.h"
#include "tasklet.h"
#include "application.h"
#include "buffer.h"
#include "http_reader.h"

struct http_client {
	struct mutex mutex;
	struct tasklet tasklet;
	struct socket *socket;
	struct error err;
	struct growbuf request;
	struct drainbuf request_out;
	struct http_reader reader;
};

static void write_request(void *v_c);
static void read_response_prebody(void *v_c);
static void read_response_body(void *v_c);

void http_client_set_header(struct http_client *c, const char *name,
			    const char *val)
{
	growbuf_append_string(&c->request, name);
	growbuf_append_string(&c->request, ": ");
	growbuf_append_string(&c->request, val);
	growbuf_append_string(&c->request, "\r\n");
}

struct http_client *http_client_create(struct socket *s)
{
	struct http_client *c = xalloc(sizeof *c);

	mutex_init(&c->mutex);
	tasklet_init(&c->tasklet, &c->mutex, c);
	c->socket = s;
	error_init(&c->err);
	http_reader_init(&c->reader, s, FALSE);

	growbuf_init(&c->request, 1000);
	growbuf_printf(&c->request, "GET %s HTTP/1.1\r\n", "/");
	http_client_set_header(c, "Connection", "close");

	mutex_lock(&c->mutex);
	tasklet_now(&c->tasklet, write_request);
	return c;
}

void http_client_destroy(struct http_client *c)
{
	mutex_lock(&c->mutex);
	tasklet_fini(&c->tasklet);
	socket_destroy(c->socket);
	error_fini(&c->err);
	http_reader_fini(&c->reader);
	growbuf_fini(&c->request);
	mutex_unlock_fini(&c->mutex);
	free(c);
}

static void write_request(void *v_c)
{
	struct http_client *c = v_c;
	ssize_t res;

	if (!growbuf_frozen(&c->request)) {
		growbuf_append_string(&c->request, "\r\n");
		growbuf_to_drainbuf(&c->request, &c->request_out);
	}

	for (;;) {
		size_t len = drainbuf_length(&c->request_out);

		if (!len) {
			socket_partial_close(c->socket, 1, 0, &c->err);
			tasklet_now(&c->tasklet, read_response_prebody);
			return;
		}

		res = socket_write(c->socket, drainbuf_current(&c->request_out),
				   len, &c->tasklet, &c->err);
		if (res < 0)
			break;

		drainbuf_advance(&c->request_out, res);
	}

	if (!error_ok(&c->err)) {
		fprintf(stderr, "Error: %s\n", error_message(&c->err));
		tasklet_stop(&c->tasklet);
	}

	mutex_unlock(&c->mutex);
}

static void read_response_prebody(void *v_c)
{
	struct http_client *c = v_c;

	switch (http_reader_prebody(&c->reader, &c->tasklet, &c->err)) {
	case HTTP_READER_PREBODY_BLOCKED:
		mutex_unlock(&c->mutex);
		return;

	case HTTP_READER_PREBODY_DONE:
		tasklet_now(&c->tasklet, read_response_body);
		return;

	case HTTP_READER_PREBODY_CLOSED:
		break;

	case HTTP_READER_PREBODY_ERROR:
		goto error;
	}

	socket_close(c->socket, &c->err);
	fprintf(stderr, "Connection done\n");

 error:
	if (!error_ok(&c->err))
		fprintf(stderr, "Error: %s\n", error_message(&c->err));

	tasklet_stop(&c->tasklet);
	application_stop();
	mutex_unlock(&c->mutex);
}

static void read_response_body(void *v_c)
{
	struct http_client *c = v_c;
	char buf[100];

	for (;;) {
		ssize_t res = http_reader_body(&c->reader, buf, 100,
					       &c->tasklet, &c->err);
		if (res < 0) {
			if (!error_ok(&c->err)) {
				fprintf(stderr, "Error: %s\n",
					error_message(&c->err));
				break;
			}
			else {
				/* blocked */
				mutex_unlock(&c->mutex);
				return;
			}
		}

		if (res == 0)
			break;

		printf("%.*s", (int)res, buf);
	}

	tasklet_stop(&c->tasklet);
	application_stop();
	mutex_unlock(&c->mutex);
}

int main(int argc, char **argv)
{
	struct error err;
	struct socket_factory *sf = socket_factory();
	struct socket *s;
	struct http_client *hc;
	int res = 0;

	error_init(&err);

	if (argc != 3) {
		error_set(&err, ERROR_MISC,
			  "usage: %s <host> <service>", argv[0]);
		goto out;
	}

	application_prepare();
	s = socket_factory_connect(sf, argv[1], argv[2], &err);
	if (!error_ok(&err))
		goto out;

	hc = http_client_create(s);
	application_run();
	http_client_destroy(hc);

 out:
	res = !error_ok(&err);
	if (res) {
		fprintf(stderr, "%s\n", error_message(&err));
		res = 1;
	}

	error_fini(&err);
	return res;
}
