#include <stdio.h>

#include <molerat/socket.h>
#include <molerat/tasklet.h>
#include <molerat/timer.h>

#include <molerat/http_server.h>
#include <molerat/http_reader.h>
#include <molerat/http_writer.h>

struct http_server {
	http_server_handler_t handler;
	void *handler_data;

	struct mutex mutex;
	struct tasklet tasklet;
	struct server_socket *server_socket;

	struct http_server_exchange **connections;
	size_t connections_used;
	size_t connections_size;
};

struct http_server_exchange {
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
};

static void http_server_accept(void *v_hs);

struct http_server *http_server_create(struct server_socket *s,
				       http_server_handler_t cb, void *data)
{
	struct http_server *hs = xalloc(sizeof *hs);

	hs->handler = cb;
	hs->handler_data = data;

	mutex_init(&hs->mutex);
	tasklet_init(&hs->tasklet, &hs->mutex, hs);
	hs->server_socket = s;

	hs->connections_used = 0;
	hs->connections_size = 10;
	hs->connections
		= xalloc(hs->connections_size * sizeof *hs->connections);

	mutex_lock(&hs->mutex);
	tasklet_goto(&hs->tasklet, http_server_accept);
	mutex_unlock(&hs->mutex);

	return hs;
}

static void add_connection(struct http_server *server,
			   struct http_server_exchange *conn)
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

static void remove_connection(struct http_server_exchange *conn)
{
	struct http_server *server = conn->server;

	mutex_lock(&server->mutex);
	server->connections[conn->index]
		= server->connections[--server->connections_used];
	server->connections[conn->index]->index = conn->index;
	mutex_unlock(&server->mutex);
}

static void connection_create(struct http_server *server, struct socket *s);
static void connection_destroy_locked(struct http_server_exchange *conn);

void http_server_destroy(struct http_server *hs)
{
	/* Stop accepting new connections */
	mutex_lock(&hs->mutex);
	tasklet_fini(&hs->tasklet);
	server_socket_destroy(hs->server_socket);

	/* Close all existing connections */
	while (hs->connections_used) {
		struct http_server_exchange *conn = hs->connections[0];

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
		announce_connection(s);
		connection_create(hs, s);
	}

	if (!error_ok(&err))
		fprintf(stderr, "%s\n", error_message(&err));

	error_fini(&err);
}

static void connection_read_prebody_handler(void *v_c);
static void connection_timeout(void *v_c);

static void update_timeout(struct http_server_exchange *c)
{
	timer_set_relative(&c->timeout, 240 * XTIME_SECOND, 241 * XTIME_SECOND);
}

static void connection_create(struct http_server *server, struct socket *s)
{
	struct http_server_exchange *conn = xalloc(sizeof *conn);

	mutex_init(&conn->mutex);
	conn->socket = s;
	tasklet_init(&conn->tasklet, &conn->mutex, conn);
	timer_init(&conn->timeout);
	update_timeout(conn);
	tasklet_init(&conn->timeout_tasklet, &conn->mutex, conn);
	error_init(&conn->err);
	http_reader_init_request(&conn->reader, socket_stream(s));
	http_writer_init(&conn->writer, socket_stream(s));
	add_connection(server, conn);

	mutex_lock(&conn->mutex);
	tasklet_later(&conn->tasklet, connection_read_prebody_handler);
	tasklet_later(&conn->timeout_tasklet, connection_timeout);
	mutex_unlock(&conn->mutex);
}

static void connection_destroy_locked(struct http_server_exchange *conn)
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

static bool_t connection_read_prebody(struct http_server_exchange *c)
{
	switch (http_reader_prebody(&c->reader, &c->tasklet, &c->err)) {
	case HTTP_READER_PREBODY_PROGRESS:
		update_timeout(c);
		/* Fall through */

	case HTTP_READER_PREBODY_WAITING:
		return TRUE;

	case HTTP_READER_PREBODY_DONE:
		timer_cancel(&c->timeout);
		tasklet_stop(&c->tasklet);
		c->server->handler(c->server->handler_data,
				   c, &c->reader, &c->writer);
		return TRUE;

	case HTTP_READER_PREBODY_CLOSED:
		break;

	case HTTP_READER_PREBODY_ERROR:
		goto error;
	}

	switch (socket_close(c->socket, &c->tasklet, &c->err)) {
	case STREAM_OK:
		fprintf(stderr, "Connection done\n");
		goto stop;

	case STREAM_WAITING:
		return TRUE;

	default:
		break;
	}

 error:
	fprintf(stderr, "Error: %s\n", error_message(&c->err));

 stop:
	connection_destroy_locked(c);
	return FALSE;
}

static void connection_read_prebody_handler(void *v_c)
{
	connection_read_prebody(v_c);
}

void http_server_exchange_done(struct http_server_exchange *c,
			       struct error *err)
{
	mutex_lock(&c->mutex);

	if (!err) {
		update_timeout(c);
		tasklet_set_handler(&c->tasklet,
				    connection_read_prebody_handler);
		if (connection_read_prebody(c))
			mutex_unlock(&c->mutex);
	}
	else {
		connection_destroy_locked(c);
	}
}

static void connection_timeout(void *v_c)
{
	struct http_server_exchange *c = v_c;

	if (!timer_wait(&c->timeout, &c->timeout_tasklet))
		return;

	fprintf(stderr, "Closing connection due to timeout\n");
	connection_destroy_locked(c);
}
