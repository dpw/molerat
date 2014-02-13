#include <assert.h>

#include <molerat/transport.h>
#include <molerat/socket.h>
#include <molerat/tasklet.h>

static struct address_ops socket_address_ops;
static struct async_server_ops socket_server_ops;
static struct async_transport_ops socket_transport_ops;

struct socket_transport {
	struct async_transport base;
	struct socket_factory *socket_factory;
};

struct socket_server {
	struct async_server base;
	async_message_handler_t handler;
	void *handler_data;
	struct server_socket *socket;
	struct mutex mutex;
	struct tasklet tasklet;
};

struct socket_address {
	struct address base;
	struct sockaddr **addrs;
};

struct async_transport *socket_transport_create(struct socket_factory *sf)
{
	struct socket_transport *t = xalloc(sizeof *t);

	t->base.ops = &socket_transport_ops;
	t->socket_factory = sf;

	return &t->base;
}

static void st_destroy(struct async_transport *gt)
{
	struct socket_transport *t
		= container_of(gt, struct socket_transport, base);
	free(t);
}

static struct stream *st_send(struct async_transport *gt,
			      struct address *gaddr, struct error *err)
{
	struct socket_transport *t
		= container_of(gt, struct socket_transport, base);
	struct socket_address *addr;

	assert(gaddr->ops == &socket_address_ops);
	addr = container_of(gaddr, struct socket_address, base);
	return socket_stream(socket_factory_connect_addresses(t->socket_factory,
							 addr->addrs, err));
}

static void ss_accept(void *v_s)
{
	struct socket_server *s = v_s;
	struct error err;
	struct socket *sock;

	error_init(&err);

	while ((sock = server_socket_accept(s->socket, &s->tasklet,
					    &err)))
		s->handler(socket_stream(sock), s->handler_data);

	if (!error_ok(&err)) {
		warn("%s", error_message(&err));
		tasklet_stop(&s->tasklet);
	}

	error_fini(&err);
}

static struct async_server *st_serve(struct async_transport *gt,
				     async_message_handler_t handler,
				     void *data,
				     struct error *err)
{
	struct socket_transport *t
		= container_of(gt, struct socket_transport, base);
	struct socket_server *s = xalloc(sizeof *s);

	s->base.ops = &socket_server_ops;
	s->handler = handler;
	s->handler_data = data;
	s->socket
		= socket_factory_unbound_server_socket(t->socket_factory, err);
	if (unlikely(!s->socket)) {
		free(s);
		return NULL;
	}

	mutex_init(&s->mutex);
	tasklet_init(&s->tasklet, &s->mutex, s);

	mutex_lock(&s->mutex);
	tasklet_goto(&s->tasklet, ss_accept);
	mutex_unlock(&s->mutex);

	return &s->base;
}

static void ss_destroy(struct async_server *gs)
{
	struct socket_server *s = container_of(gs, struct socket_server, base);

	mutex_lock(&s->mutex);
	tasklet_fini(&s->tasklet);
	mutex_unlock_fini(&s->mutex);
	server_socket_destroy(s->socket);
	free(s);
}

static struct address *ss_get_address(struct async_server *gs,
				      struct error *err)
{
	struct socket_server *s = container_of(gs, struct socket_server, base);
	struct socket_address *a = xalloc(sizeof *a);

	a->base.ops = &socket_address_ops;
	a->addrs = server_socket_addresses(s->socket, err);
	if (a->addrs)
		return &a->base;

	free(a);
	return NULL;
}

static struct async_transport_ops socket_transport_ops = {
	st_destroy,
	st_serve,
	st_send
};

static struct async_server_ops socket_server_ops = {
	ss_destroy,
	ss_get_address
};


static void sa_release(struct address *ga)
{
	struct socket_address *a
		= container_of(ga, struct socket_address, base);

	free_sockaddrs(a->addrs);
	free(a);
}

static struct address_ops socket_address_ops = {
	sa_release
};
