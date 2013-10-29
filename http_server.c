#include <stdio.h>
#include <string.h>

#include "socket.h"
#include "tasklet.h"
#include "http-parser/http_parser.h"

#include "http_server.h"
#include "http_reader.h"

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
	struct socket *socket;
	struct error err;
	struct http_reader reader;
	char *write_buf;
	size_t write_len;
	size_t write_pos;
};

static void http_server_accept(void *v_hs);

struct http_server *http_server_create(struct server_socket *s,
				       bool_t verbose, struct error *err)
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
	tasklet_now(&hs->tasklet, http_server_accept);

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

static void connection_write(void *v_c);
static void connection_read_body(void *v_c);
static void connection_read_prebody(void *v_c);

static struct connection *connection_create(struct http_server *server,
					    struct socket *s)
{
	struct connection *conn = xalloc(sizeof *conn);

	mutex_init(&conn->mutex);
	conn->socket = s;
	tasklet_init(&conn->tasklet, &conn->mutex, conn);
	error_init(&conn->err);
	http_reader_init(&conn->reader, s, TRUE);
	conn->write_buf = NULL;
	add_connection(server, conn);

	mutex_lock(&conn->mutex);
	tasklet_later(&conn->tasklet, connection_read_prebody);
	mutex_unlock(&conn->mutex);

	return conn;
}

static void connection_destroy_locked(struct connection *conn)
{
	mutex_assert_held(&conn->mutex);

	remove_connection(conn);
	mutex_veto_transfer(&conn->mutex);
	tasklet_fini(&conn->tasklet);
	http_reader_fini(&conn->reader);
	socket_destroy(conn->socket);
	free(conn->write_buf);
	error_fini(&conn->err);
	mutex_unlock_fini(&conn->mutex);
	free(conn);
}

//static const char body[] = "<html><body>Hello, world!</body></html>";
static const char body[] = "<html><body><form action='/' method='post'><input type='hidden' name='foo' value='bar'><input type='submit'></form></body></html>";

static void construct_response(struct connection *c)
{
	assert(!c->write_buf);

	c->write_len = xsprintf(&c->write_buf,
			"HTTP/1.1 200 OK\r\n"
			/*"Date: Thu, 24 May 2012 15:14:21 GMT\r\n"*/
			"Server: MoleRat\r\n"
			"Last-Modified: Fri, 30 Dec 2011 21:35:47 GMT\r\n"
			"Content-Length: %lu\r\n"
			"Content-Type: text/html; charset=utf-8\r\n"
			"\r\n"
			"%s",
			(unsigned long)strlen(body), body);
	c->write_pos = 0;
}

static void connection_read_prebody(void *v_c)
{
	struct connection *c = v_c;

	switch (http_reader_prebody(&c->reader, &c->tasklet, &c->err)) {
	case HTTP_READER_PREBODY_BLOCKED:
		mutex_unlock(&c->mutex);
		return;

	case HTTP_READER_PREBODY_DONE:
		construct_response(c);
		tasklet_now(&c->tasklet, connection_read_body);
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

	connection_destroy_locked(c);
}

static void connection_read_body(void *v_c)
{
	char buf[100];
	struct connection *c = v_c;

	for (;;) {
		ssize_t res = http_reader_body(&c->reader, buf, 100,
					       &c->tasklet, &c->err);
		fprintf(stderr, "Read %ld body bytes\n", (long)res);

		if (res == 0) {
			tasklet_now(&c->tasklet, connection_write);
			return;
		}

		if (res < 0)
			break;
	}

	if (error_ok(&c->err)) {
		/* blocked */
		mutex_unlock(&c->mutex);
	}
	else {
		fprintf(stderr, "Error: %s\n", error_message(&c->err));
		connection_destroy_locked(c);
	}
}

static void connection_write(void *v_c)
{
	struct connection *c = v_c;

	for (;;) {
		ssize_t res = socket_write(c->socket,
					   c->write_buf + c->write_pos,
					   c->write_len - c->write_pos,
					   &c->tasklet, &c->err);
		if (res < 0)
			break;

		c->write_pos += res;
		if (c->write_pos == c->write_len) {
			free(c->write_buf);
			c->write_buf = NULL;
			tasklet_later(&c->tasklet, connection_read_prebody);
			mutex_unlock(&c->mutex);
			return;
		}
	}

	if (error_ok(&c->err)) {
		/* blocked */
		mutex_unlock(&c->mutex);
	}
	else {
		fprintf(stderr, "Error: %s\n", error_message(&c->err));
		connection_destroy_locked(c);
	}
}
