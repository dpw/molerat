#include <stdio.h>

#include "socket.h"
#include "tasklet.h"
#include "buffer.h"

struct http_client {
	struct mutex mutex;
	struct tasklet tasklet;
	struct socket *socket;
	struct error err;
	struct growbuf request;
	struct drainbuf request_out;
};

static void write_request(void *v_c);
static void read_response(void *v_c);

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
			tasklet_now(&c->tasklet, read_response);
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

static void read_response(void *v_c)
{
	struct http_client *c = v_c;
	ssize_t res;
	char buf[100];

	for (;;) {
		res = socket_read(c->socket, buf, 100, &c->tasklet, &c->err);
		if (res <= 0)
			break;

		printf("%.*s", (int)res, buf);
	}

	if (res == 0) {
		socket_close(c->socket, &c->err);
		printf("\n");
		fprintf(stderr, "Connection done\n");
		tasklet_stop(&c->tasklet);
		socket_factory_stop();
	}

	if (!error_ok(&c->err)) {
		fprintf(stderr, "Error: %s\n", error_message(&c->err));
		tasklet_stop(&c->tasklet);
	}

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

	s = socket_factory_connect(sf, argv[1], argv[2], &err);
	if (!error_ok(&err))
		goto out;

	hc = http_client_create(s);
	socket_factory_run(sf, &err);
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
