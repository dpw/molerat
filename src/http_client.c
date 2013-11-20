#include <stdio.h>

#include <molerat/socket.h>
#include <molerat/tasklet.h>
#include <molerat/application.h>
#include <molerat/http_reader.h>
#include <molerat/http_writer.h>

struct http_client {
	struct mutex mutex;
	struct tasklet tasklet;
	struct socket *socket;
	struct error err;
	struct http_writer writer;
	struct http_reader reader;
};

static void write_request(void *v_c);
static void read_response_prebody(void *v_c);
static void read_response_body(void *v_c);

struct http_client *http_client_create(struct socket *s, const char *host)
{
	struct http_client *c = xalloc(sizeof *c);

	mutex_init(&c->mutex);
	tasklet_init(&c->tasklet, &c->mutex, c);
	c->socket = s;
	error_init(&c->err);
	http_writer_init(&c->writer, socket_stream(s));
	http_reader_init(&c->reader, socket_stream(s), FALSE);

	http_writer_request(&c->writer, "/");
	http_writer_header(&c->writer, "Connection", "close");
	http_writer_header(&c->writer, "Host", host);

	mutex_lock(&c->mutex);
	tasklet_goto(&c->tasklet, write_request);
	return c;
}

void http_client_destroy(struct http_client *c)
{
	mutex_lock(&c->mutex);
	tasklet_fini(&c->tasklet);
	socket_destroy(c->socket);
	error_fini(&c->err);
	http_reader_fini(&c->reader);
	http_writer_fini(&c->writer);
	mutex_unlock_fini(&c->mutex);
	free(c);
}

static void write_request(void *v_c)
{
	struct http_client *c = v_c;
	ssize_t res = http_writer_end(&c->writer, &c->tasklet, &c->err);
	switch (res) {
	case HTTP_WRITER_END_ERROR:
		goto error;

	case HTTP_WRITER_END_WAITING:
		goto out;

	case HTTP_WRITER_END_DONE:
		if (!socket_close_write(c->socket, &c->err))
			goto error;

		tasklet_goto(&c->tasklet, read_response_prebody);
		return;
	}

 error:
	fprintf(stderr, "Error: %s\n", error_message(&c->err));
	tasklet_stop(&c->tasklet);
	application_stop();
 out:
	mutex_unlock(&c->mutex);
}

static void read_response_prebody(void *v_c)
{
	struct http_client *c = v_c;

	switch (http_reader_prebody(&c->reader, &c->tasklet, &c->err)) {
	case HTTP_READER_PREBODY_WAITING:
	case HTTP_READER_PREBODY_PROGRESS:
		mutex_unlock(&c->mutex);
		return;

	case HTTP_READER_PREBODY_DONE:
		tasklet_goto(&c->tasklet, read_response_body);
		return;

	case HTTP_READER_PREBODY_CLOSED:
		break;

	case HTTP_READER_PREBODY_ERROR:
		goto error;
	}

	if (!socket_close(c->socket, &c->err))
		goto error;

	fprintf(stderr, "Connection done\n");
	goto out;

 error:
	fprintf(stderr, "Error: %s\n", error_message(&c->err));

 out:
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
		switch (res) {
		case STREAM_WAITING:
			mutex_unlock(&c->mutex);
			return;

		case STREAM_ERROR:
			goto error;

		case STREAM_END:
			goto done;
		}

		printf("%.*s", (int)res, buf);
	}

 error:
	fprintf(stderr, "Error: %s\n", error_message(&c->err));

 done:
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

	hc = http_client_create(s, argv[1]);
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
