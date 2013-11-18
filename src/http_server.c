#include <stdio.h>
#include <string.h>

#include "socket.h"
#include "tasklet.h"
#include "timer.h"

#include "http_server.h"
#include "http_reader.h"
#include "http_writer.h"

struct http_server {
	struct mutex mutex;
	struct tasklet tasklet;
	struct server_socket *server_socket;
	bool_t verbose;

	struct connection **connections;
	size_t connections_used;
	size_t connections_size;
};

struct connection {
	struct http_server *server;
	size_t index;

	struct mutex mutex;
	struct tasklet tasklet;
	struct timer timeout;
	struct tasklet timeout_tasklet;
	struct socket *socket;
	struct error err;
	struct http_reader reader;
	struct http_writer writer;
	size_t body_len;
	size_t body_pos;
};

static void http_server_accept(void *v_hs);

struct http_server *http_server_create(struct server_socket *s,
				       bool_t verbose)
{
	struct http_server *hs;

	hs = xalloc(sizeof *hs);
	mutex_init(&hs->mutex);
	tasklet_init(&hs->tasklet, &hs->mutex, hs);
	hs->server_socket = s;
	hs->verbose = verbose;

	hs->connections_used = 0;
	hs->connections_size = 10;
	hs->connections
		= xalloc(hs->connections_size * sizeof *hs->connections);

	mutex_lock(&hs->mutex);
	tasklet_goto(&hs->tasklet, http_server_accept);

	return hs;
}

static void add_connection(struct http_server *server, struct connection *conn)
{
	size_t index = server->connections_used;

	mutex_assert_held(&server->mutex);

	if (++server->connections_used > server->connections_size) {
		server->connections_size *= 2;
		server->connections = xrealloc(server->connections,
			server->connections_size * sizeof *server->connections);
	}

	server->connections[index] = conn;
	conn->server = server;
	conn->index = index;
}

static void remove_connection(struct connection *conn)
{
	struct http_server *server = conn->server;

	mutex_lock(&server->mutex);
	server->connections[conn->index]
		= server->connections[--server->connections_used];
	server->connections[conn->index]->index = conn->index;
	mutex_unlock(&server->mutex);
}

static struct connection *connection_create(struct http_server *server,
					    struct socket *s);
static void connection_destroy_locked(struct connection *conn);

void http_server_destroy(struct http_server *hs)
{
	/* Stop accepting new connections */
	mutex_lock(&hs->mutex);
	tasklet_fini(&hs->tasklet);
	server_socket_destroy(hs->server_socket);

	/* Close all existing connections */
	for (;;) {
		struct connection *conn;

		if (!hs->connections_used)
			break;

		conn = hs->connections[0];
		if (mutex_transfer(&hs->mutex, &conn->mutex)) {
			connection_destroy_locked(conn);
			mutex_lock(&hs->mutex);
		}
	}

	mutex_unlock_fini(&hs->mutex);
	free(hs->connections);
	free(hs);
}

static void announce_connection(struct socket *s)
{
	struct error err;
	struct sockaddr *sa;
	char *printed;

	error_init(&err);

	sa = socket_peer_address(s, &err);
	if (sa) {
		printed = print_sockaddr(sa, &err);
		if (printed) {
			fprintf(stderr, "Connection from %s\n", printed);
			free(printed);
		}

		free(sa);
	}

	if (!error_ok(&err))
		fprintf(stderr, "%s\n", error_message(&err));

	error_fini(&err);
}

static void http_server_accept(void *v_hs)
{
	struct http_server *hs = v_hs;
	struct error err;
	struct socket *s;

	error_init(&err);

	while ((s = server_socket_accept(hs->server_socket, &hs->tasklet,
					 &err))) {
		if (hs->verbose)
			announce_connection(s);

		connection_create(hs, s);
	}

	if (!error_ok(&err))
		fprintf(stderr, "%s\n", error_message(&err));

	mutex_unlock(&hs->mutex);
	error_fini(&err);
}

static void connection_read_body(void *v_c);
static void connection_read_prebody(void *v_c);
static void respond(struct connection *c);
static void connection_write(void *v_c);
static void connection_timeout(void *v_c);

static void update_timeout(struct connection *c)
{
	timer_set_relative(&c->timeout, 240 * XTIME_SECOND, 241 * XTIME_SECOND);
}

static struct connection *connection_create(struct http_server *server,
					    struct socket *s)
{
	struct connection *conn = xalloc(sizeof *conn);

	mutex_init(&conn->mutex);
	conn->socket = s;
	tasklet_init(&conn->tasklet, &conn->mutex, conn);
	timer_init(&conn->timeout);
	update_timeout(conn);
	tasklet_init(&conn->timeout_tasklet, &conn->mutex, conn);
	error_init(&conn->err);
	http_reader_init(&conn->reader, socket_stream(s), TRUE);
	http_writer_init(&conn->writer, socket_stream(s));
	add_connection(server, conn);

	tasklet_later(&conn->tasklet, connection_read_prebody);
	mutex_lock(&conn->mutex);
	tasklet_goto(&conn->timeout_tasklet, connection_timeout);

	return conn;
}

static void connection_destroy_locked(struct connection *conn)
{
	mutex_assert_held(&conn->mutex);

	remove_connection(conn);
	mutex_veto_transfer(&conn->mutex);
	tasklet_fini(&conn->tasklet);
	timer_fini(&conn->timeout);
	tasklet_fini(&conn->timeout_tasklet);
	http_reader_fini(&conn->reader);
	http_writer_fini(&conn->writer);
	socket_destroy(conn->socket);
	error_fini(&conn->err);
	mutex_unlock_fini(&conn->mutex);
	free(conn);
}

static void dump_headers(struct http_reader *r)
{
	struct http_header_iter iter;
	struct http_header *header;

	for (http_reader_headers(r, &iter);
	     (header = http_header_iter_next(&iter));) {
		printf("Header <%.*s> <%.*s>\n",
		       (int)header->name_len, header->name,
		       (int)header->value_len, header->value);
	}
}

static void connection_read_prebody(void *v_c)
{
	struct connection *c = v_c;

	switch (http_reader_prebody(&c->reader, &c->tasklet, &c->err)) {
	case HTTP_READER_PREBODY_PROGRESS:
		update_timeout(c);
		/* Fall through */

	case HTTP_READER_PREBODY_WAITING:
		mutex_unlock(&c->mutex);
		return;

	case HTTP_READER_PREBODY_DONE:
		dump_headers(&c->reader);
		tasklet_goto(&c->tasklet, connection_read_body);
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
	connection_destroy_locked(c);
}

static void connection_read_body(void *v_c)
{
	char buf[100];
	struct connection *c = v_c;

	for (;;) {
		ssize_t res = http_reader_body(&c->reader, buf, 100,
					       &c->tasklet, &c->err);
		switch (res) {
		case STREAM_WAITING:
			mutex_unlock(&c->mutex);
			return;

		case STREAM_ERROR:
			fprintf(stderr, "Error: %s\n", error_message(&c->err));
			connection_destroy_locked(c);
			return;

		case STREAM_END:
			respond(c);
			return;

		default:
			assert(res >= 0);
			break;
		}

		fprintf(stderr, "Read %ld body bytes\n", (long)res);
	}
}

static const char body[] = "<html><body><h1>Hello from Molerat</h1><form action='/' method='post'><input type='hidden' name='foo' value='bar'><input type='submit' value='Send a POST request'></form></body></html>";


static void respond(struct connection *c)
{
	timer_cancel(&c->timeout);

	c->body_len = strlen(body);
	c->body_pos = 0;

	http_writer_response(&c->writer, 200);
	http_writer_header(&c->writer, "Server","Molerat");
	http_writer_headerf(&c->writer, "Content-Length", "%lu",
			    (unsigned long)c->body_len);
	http_writer_header(&c->writer, "Content-Type",
			   "text/html; charset=utf-8");

	tasklet_goto(&c->tasklet, connection_write);
}


static void connection_write(void *v_c)
{
	struct connection *c = v_c;

	while (c->body_pos < c->body_len) {
		ssize_t res = http_writer_write(&c->writer,
						body + c->body_pos,
						c->body_len - c->body_pos,
						&c->tasklet, &c->err);
		switch (res) {
		case STREAM_WAITING:
			mutex_unlock(&c->mutex);
			return;

		case STREAM_ERROR:
			fprintf(stderr, "Error: %s\n", error_message(&c->err));
			connection_destroy_locked(c);
			return;

		default:
			assert(res >= 0);
			break;
		}

		c->body_pos += res;
	}

	tasklet_later(&c->tasklet, connection_read_prebody);
	mutex_unlock(&c->mutex);
}

static void connection_timeout(void *v_c)
{
	struct connection *c = v_c;

	if (!timer_wait(&c->timeout, &c->timeout_tasklet)) {
		mutex_unlock(&c->mutex);
		return;
	}

	fprintf(stderr, "Closing connection due to timeout\n");
	connection_destroy_locked(c);
}
