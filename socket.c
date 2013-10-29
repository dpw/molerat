#include <assert.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>

#include "socket.h"
#include "tasklet.h"

static int blocked(void)
{
	switch (errno) {
		// POSIX says EWOULDBLOCK can be distinct from EAGAIN
#if EWOULDBLOCK != EAGAIN
	case EWOULDBLOCK:
#endif
	case EAGAIN:
	case EINPROGRESS:
		return 1;
	}

	return 0;
}

static int make_socket(int family, int socktype, struct error *err)
{
	int fd = socket(family, socktype, 0);
	if (fd >= 0) {
		if (fcntl(fd, F_SETFL, O_NONBLOCK) >= 0)
			return fd;

		error_errno(err, "fcntl");
		close(fd);
	}
	else {
		error_errno(err, "socket");
	}

	return -1;
}

size_t sockaddr_len(struct sockaddr *sa, struct error *err)
{
	switch (sa->sa_family) {
	case AF_INET:
		return sizeof(struct sockaddr_in);

	case AF_INET6:
		return sizeof(struct sockaddr_in6);

	default:
		error_set(err, ERROR_INVALID,
			  "strange socket address family %d",
			  (int)sa->sa_family);
		return 0;
	}
}

static struct sockaddr *copy_sockaddr(struct sockaddr_storage *ss,
				      socklen_t len,
				      struct error *err)
{
	size_t expected_len = sockaddr_len((struct sockaddr *)ss, err);
	if (expected_len) {
		if (len == expected_len) {
			struct sockaddr *sa = xalloc(expected_len);
			memcpy(sa, ss, expected_len);
			return sa;
		}

		error_set(err, ERROR_INVALID,
			  "strange socket address length for family %d"
			                                       " (%u bytes)",
			  (int)ss->ss_family, (unsigned int)len);
	}

	return NULL;
}

static struct sockaddr *get_socket_address(int fd, struct error *err)
{
	struct sockaddr_storage ss;
	socklen_t len = sizeof ss;

	if (getsockname(fd, (struct sockaddr *)&ss, &len) >= 0)
		return copy_sockaddr(&ss, len, err);

	error_errno(err, "getsockname");
	return NULL;
}

static struct sockaddr *get_socket_peer_address(int fd, struct error *err)
{
	struct sockaddr_storage ss;
	socklen_t len = sizeof ss;

	if (getpeername(fd, (struct sockaddr *)&ss, &len) >= 0)
		return copy_sockaddr(&ss, len, err);

	error_errno(err, "getpeername");
	return NULL;
}

char *print_sockaddr(struct sockaddr *sa, struct error *err)
{
	char buf[INET6_ADDRSTRLEN];
	int res;
	void *addr;
	char *printed;
	const char *format;
	int port;

	switch (sa->sa_family) {
	case AF_INET: {
		struct sockaddr_in *sin = (struct sockaddr_in *)sa;
		format = "%s:%d";
		addr = &sin->sin_addr;
		port = sin->sin_port;
		break;
	}

	case AF_INET6: {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sa;
		format = "[%s]:%d";
		addr = &sin6->sin6_addr;
		port = sin6->sin6_port;
		break;
	}

	default:
		error_set(err, ERROR_INVALID,
			  "unknown socket address family %d",
			  (int)sa->sa_family);
		return NULL;
	}

	if (!inet_ntop(sa->sa_family, addr, buf, INET6_ADDRSTRLEN)) {
		error_errno(err, "inet_ntop");
		return NULL;
	}

	res = asprintf(&printed, format, buf, ntohs(port));
	assert(res >= 0);
	return printed;
}

void free_sockaddrs(struct sockaddr **addrs)
{
	struct sockaddr **p = addrs;
	if (p)
		while (*p)
			free(*p++);

	free(addrs);
}


struct simple_socket_factory {
	struct socket_factory base;
	struct mutex mutex;

	/* pollfs is the array of pollfds that gets passed to poll(2).
	 *
	 * We don't want to modify pollfds while poll is being called,
	 * or while the events are being dispatched (which gets
	 * complicated if pollfd entries are moving around).  Instead,
	 * updates to pollfds are gathered in pollfd_updates, and
	 * pollfds only gets modified within apply_updates.
	 *
	 * pollfd_sockets records the sockets corresponding to pollfds
	 * entries.
	 */
	struct pollfd *pollfds;
	struct common_socket **pollfd_sockets;
	unsigned int pollfds_size;
	unsigned int pollfds_used;

	struct pollfd_update *pollfd_updates;
	unsigned int pollfd_updates_size;
	unsigned int pollfd_updates_used;
};

struct common_socket {
	struct socket base;
	int fd;

	/* The sign of slot indicates what it refers to:
	 *
	 * >0: There is an entry in pollfds for the socket, at
	 * pollfds[slot-1].
	 *
	 * <0: There is an entry in pollfd_updates for the socket, at
	 * pollfds[~slot].  There may or may not be a pollfd entry
	 * too.
	 *
	 * 0: There is no pollfd or pollfd_update for this socket.
	 */
	int slot;
};

struct pollfd_update {
	/* The index of the entry in pollfds to which this update
	   applies, or -1 if it is new. */
	int poll_slot;
	short events;
	struct common_socket *socket;
};


static void common_socket_init(struct common_socket *s,
			       struct socket_ops *ops, int fd)
{
	s->base.ops = ops;
	s->fd = fd;
	s->slot = 0;
}

static void common_socket_close(struct common_socket *s, struct error *e)
{
	int fd = s->fd;
	s->fd = -1;

	if (fd >= 0 && close(fd) < 0)
		error_errno(e, "close");
}

static void common_socket_fini(struct common_socket *s,
			       struct simple_socket_factory *f)
{
	mutex_assert_held(&f->mutex);

	/* Clear any pointers to this socket */
	if (s->slot < 0) {
		struct pollfd_update *u = &f->pollfd_updates[~s->slot];
		assert(u->events == 0);
		u->socket = NULL;
		if (u->poll_slot >= 0)
			f->pollfd_sockets[u->poll_slot] = NULL;
	}
	else if (s->slot > 0) {
		f->pollfd_sockets[s->slot - 1] = NULL;
	}
}

static struct socket_factory_ops simple_socket_factory_ops;

static struct socket_factory *socket_factory_create(void)
{
	struct simple_socket_factory *f = xalloc(sizeof *f);
	f->base.ops = &simple_socket_factory_ops;
	mutex_init(&f->mutex);

	f->pollfds = NULL;
	f->pollfd_sockets = NULL;
	f->pollfds_size = f->pollfds_used = 0;

	f->pollfd_updates = NULL;
	f->pollfd_updates_size = f->pollfd_updates_used = 0;

	return &f->base;
}

static void socket_factory_destroy(struct socket_factory *gf)
{
	struct simple_socket_factory *f = (struct simple_socket_factory *)gf;
	assert(gf->ops == &simple_socket_factory_ops);

	mutex_fini(&f->mutex);
	free(f->pollfds);
	free(f->pollfd_sockets);
	free(f->pollfd_updates);
	free(f);
}

static void set_events(struct simple_socket_factory *f,
		       struct common_socket *s, short events)
{
	mutex_assert_held(&f->mutex);

	if (s->slot >= 0) {
		/* Allocate a new update slot */
		int slot = f->pollfd_updates_used++;
		int sz = f->pollfd_updates_size;
		struct pollfd_update *update;

		if (slot == sz) {
			/* Grow pollfd_updates */
			f->pollfd_updates_size = sz = sz * 2 + 10;
			f->pollfd_updates = xrealloc(f->pollfd_updates,
					     sz * sizeof(struct pollfd_update));
		}

		update = &f->pollfd_updates[slot];
		update->poll_slot = s->slot - 1;
		update->events = events;
		update->socket = s;
		s->slot = ~slot;
	}
	else {
		/* Update the existing update slot */
		f->pollfd_updates[~s->slot].events = events;
	}
}

static void add_pollfd(struct simple_socket_factory *f,
		       struct common_socket *s, short events)
{
	int slot = f->pollfds_used++;
	int sz = f->pollfds_size;

	mutex_assert_held(&f->mutex);

	if (slot == sz) {
		/* Grow pollfds */
		f->pollfds_size = sz = sz * 2 + 10;
		f->pollfds = xrealloc(f->pollfds, sz * sizeof(struct pollfd));
		f->pollfd_sockets = xrealloc(f->pollfd_sockets,
					  sz * sizeof(struct common_socket *));
	}

	f->pollfds[slot].fd = s->fd;
	f->pollfds[slot].events = events;
	f->pollfd_sockets[slot] = s;
	s->slot = slot + 1;
}

static void delete_pollfd(struct simple_socket_factory *f, int slot)
{
	int fd;

	mutex_assert_held(&f->mutex);

	/* Copy the last pollfd over the one to be deleted */
	f->pollfds_used--;
	f->pollfds[slot] = f->pollfds[f->pollfds_used];
	f->pollfd_sockets[slot] = f->pollfd_sockets[f->pollfds_used];

	/* Now fix up the slot reference, which could be in the
	   common_socket or in the pollfd_update */
	fd = f->pollfds[slot].fd;
	if (fd >= 0)
		f->pollfd_sockets[slot]->slot = slot + 1;
	else
		f->pollfd_updates[~fd].poll_slot = slot;
}

static void apply_updates(struct simple_socket_factory *f)
{
	int i;

	mutex_assert_held(&f->mutex);

	/* We abuse the 'fd' field in pollfds during the update:
	 * Because we can shuffle the entries in f->pollfds below, we
	 * need to keep the 'slot' fields in the pollfd_updates in
	 * sync.  But that requires acess to the pollfd_update entry
	 * given a pollfd.  We use negative values in the 'fd' to
	 * provide that path. */
	for (i = 0; i < f->pollfd_updates_used; i++) {
		int poll_slot = f->pollfd_updates[i].poll_slot;
		if (poll_slot >= 0)
			f->pollfds[poll_slot].fd = ~i;
	}

	for (i = 0; i < f->pollfd_updates_used; i++) {
		struct pollfd_update *u = &f->pollfd_updates[i];
		if (u->poll_slot >= 0) {
			/* The case where a pollfd exists for this socket */
			if (u->events) {
				struct pollfd *pollfd
					= &f->pollfds[u->poll_slot];
				pollfd->events = u->events;
				pollfd->fd = u->socket->fd;
				u->socket->slot = u->poll_slot + 1;
			}
			else {
				delete_pollfd(f, u->poll_slot);
				if (u->socket)
					u->socket->slot = 0;
			}
		}
		else if (u->events) {
			/* The is no existing pollfd for this socket.
			   If the events is zero, we don't need to do
			   anything at all. */
			add_pollfd(f, u->socket, u->events);
		}

	}

	f->pollfd_updates_used = 0;
}

struct simple_socket {
	struct common_socket common;
	struct simple_socket_factory *factory;
	struct wait_list reading;
	struct wait_list writing;
	short events;
};

static struct socket_ops simple_socket_ops;

static void simple_socket_init(struct simple_socket *s,
			       struct socket_ops *ops,
			       struct simple_socket_factory *f,
			       int fd)
{
	common_socket_init(&s->common, ops, fd);
	s->factory = f;
	wait_list_init(&s->reading, 0);
	wait_list_init(&s->writing, 0);
	s->events = 0;
}

static struct socket *simple_socket_create(struct simple_socket_factory *f,
					   int fd)
{
	struct simple_socket *s = xalloc(sizeof *s);
	simple_socket_init(s, &simple_socket_ops, f, fd);
	return &s->common.base;
}

static void simple_socket_close_locked(struct simple_socket *s,
				       struct error *e)
{
	s->events = 0;
	set_events(s->factory, &s->common, 0);
	common_socket_close(&s->common, e);
}

static void simple_socket_close(struct socket *gs, struct error *e)
{
	struct simple_socket *s = (struct simple_socket *)gs;
	assert(gs->ops == &simple_socket_ops);

	mutex_lock(&s->factory->mutex);
	simple_socket_close_locked(s, e);
	mutex_unlock(&s->factory->mutex);
}

static void simple_socket_fini(struct simple_socket *s)
{
	mutex_assert_held(&s->factory->mutex);

	if (s->common.fd >= 0) {
		struct error err;
		error_init(&err);
		simple_socket_close_locked(s, &err);
		error_fini(&err);
	}

	common_socket_fini(&s->common, s->factory);
	wait_list_fini(&s->reading);
	wait_list_fini(&s->writing);
}

static void simple_socket_destroy(struct socket *gs)
{
	struct simple_socket *s = (struct simple_socket *)gs;
	assert(gs->ops == &simple_socket_ops);
	mutex_lock(&s->factory->mutex);
	simple_socket_fini(s);
	mutex_unlock(&s->factory->mutex);
	free(s);
}

static void simple_socket_handle_events(struct socket *gs, short events)
{
	struct simple_socket *s = (struct simple_socket *)gs;

	if (events & POLLIN) {
		wait_list_broadcast(&s->reading);
		s->events &= ~POLLIN;
	}

	if (events & POLLOUT) {
		wait_list_broadcast(&s->writing);
		s->events &= ~POLLOUT;
	}


	set_events(s->factory, &s->common, s->events);
}

static struct sockaddr *simple_socket_address(struct socket *gs,
					      struct error *err)
{
	struct simple_socket *s = (struct simple_socket *)gs;
	return get_socket_address(s->common.fd, err);
}

static struct sockaddr *simple_socket_peer_address(struct socket *gs,
						   struct error *err)
{
	struct simple_socket *s = (struct simple_socket *)gs;
	return get_socket_peer_address(s->common.fd, err);
}

static void simple_socket_blocked_read(struct simple_socket *s,
				       struct tasklet *t)
{
	mutex_assert_held(&s->factory->mutex);
	wait_list_wait(&s->reading, t);
}

static ssize_t simple_socket_read_locked(struct simple_socket *s,
					 void *buf, size_t len,
					 struct tasklet *t, struct error *e)
{
	if (s->common.fd >= 0) {
		ssize_t res = read(s->common.fd, buf, len);
		if (res >= 0)
			return res;

		if (blocked()) {
			simple_socket_blocked_read(s, t);
			set_events(s->factory, &s->common, s->events |= POLLIN);
		}
		else {
			error_errno(e, "read");
		}
	}
	else {
		error_set(e, ERROR_INVALID,
			  "socket_read: closed socket");
	}

	return -1;
}

static ssize_t simple_socket_read(struct socket *gs, void *buf, size_t len,
				  struct tasklet *t, struct error *e)
{
	struct simple_socket *s = (struct simple_socket *)gs;
	ssize_t res;
	assert(gs->ops == &simple_socket_ops);

	mutex_lock(&s->factory->mutex);
	res = simple_socket_read_locked(s, buf, len, t, e);
	mutex_unlock(&s->factory->mutex);
	return res;
}


static void simple_socket_blocked_write(struct simple_socket *s,
					struct tasklet *t)
{
	mutex_assert_held(&s->factory->mutex);
	wait_list_wait(&s->writing, t);
}

static ssize_t simple_socket_write_locked(struct simple_socket *s,
					  void *buf, size_t len,
					  struct tasklet *t, struct error *e)
{
	if (s->common.fd >= 0) {
		ssize_t res = write(s->common.fd, buf, len);
		if (res >= 0)
			return res;

		if (blocked()) {
			simple_socket_blocked_write(s, t);
			set_events(s->factory, &s->common,
				   s->events |= POLLOUT);
		}
		else {
			error_errno(e, "write");
		}
	}
	else {
		error_set(e, ERROR_INVALID,
			  "socket_write: closed socket");
	}

	return -1;
}

static ssize_t simple_socket_write(struct socket *gs, void *buf, size_t len,
				   struct tasklet *t, struct error *e)
{
	struct simple_socket *s = (struct simple_socket *)gs;
	ssize_t res;
	assert(gs->ops == &simple_socket_ops);

	mutex_lock(&s->factory->mutex);
	res = simple_socket_write_locked(s, buf, len, t, e);
	mutex_unlock(&s->factory->mutex);
	return res;
}

static void simple_socket_partial_close(struct socket *gs,
					bool_t writes, bool_t reads,
					struct error *e)
{
	struct simple_socket *s = (struct simple_socket *)gs;
	int how;

	if (writes) {
		if (!reads)
			how = SHUT_WR;
		else
			how = SHUT_RDWR;
	}
	else {
		if (!reads)
			return;

		how = SHUT_RD;
	}

	mutex_lock(&s->factory->mutex);

	if (shutdown(s->common.fd, how) < 0)
		error_errno(e, "shutdown");

	mutex_unlock(&s->factory->mutex);
}

static void simple_socket_wake_all(struct simple_socket *s)
{
	mutex_assert_held(&s->factory->mutex);
	wait_list_broadcast(&s->reading);
	wait_list_broadcast(&s->writing);
}

static void simple_socket_set_fd(struct simple_socket *s, int fd)
{
	assert(s->common.fd < 0);
	s->common.fd = fd;
	simple_socket_wake_all(s);
}

static struct socket_ops simple_socket_ops = {
	simple_socket_handle_events,
	simple_socket_read,
	simple_socket_write,
	simple_socket_address,
	simple_socket_peer_address,
	simple_socket_close,
	simple_socket_partial_close,
	simple_socket_destroy
};


struct connector {
	struct common_socket common;
	struct client_socket *parent;
	struct addrinfo *next_addrinfo;
	struct addrinfo *addrinfos;
	struct error err;
};

struct client_socket {
	struct simple_socket base;
	struct connector *connector;
};

static struct client_socket *client_socket_create(
					       struct simple_socket_factory *f,
					       struct connector *c);

static struct socket_ops connector_common_ops;

static struct connector *connector_create(struct simple_socket_factory *f,
					  struct addrinfo *next_addrinfo,
					  struct addrinfo *addrinfos)
{
	struct connector *c;

	c = xalloc(sizeof *c);
	c->parent = client_socket_create(f, c);
	c->next_addrinfo = next_addrinfo;
	c->addrinfos = addrinfos;
	error_init(&c->err);
	common_socket_init(&c->common, &connector_common_ops, -1);

	return c;
}

static void connector_close(struct connector *c, struct error *err)
{
	struct simple_socket_factory *f = c->parent->base.factory;
	mutex_assert_held(&f->mutex);
	set_events(f, &c->common, 0);
	common_socket_close(&c->common, err);
}

static void connector_destroy(struct connector *c)
{
	struct simple_socket_factory *f = c->parent->base.factory;
	mutex_assert_held(&f->mutex);

	if (c->common.fd >= 0) {
		struct error err;
		error_init(&err);
		connector_close(c, &err);
		error_fini(&err);
	}

	common_socket_fini(&c->common, f);

	if (c->addrinfos)
		freeaddrinfo(c->addrinfos);

	error_fini(&c->err);
	free(c);
}

static void connector_connected(struct connector *c)
{
	struct simple_socket_factory *f = c->parent->base.factory;
	mutex_assert_held(&f->mutex);
	set_events(f, &c->common, 0);
	simple_socket_set_fd(&c->parent->base, c->common.fd);
	c->parent->connector = NULL;

	c->common.fd = -1;
	connector_destroy(c);
}

static void connector_connect(struct connector *c)
{
	struct simple_socket_factory *f = c->parent->base.factory;
	mutex_assert_held(&f->mutex);

	for (;;) {
		struct addrinfo *ai = c->next_addrinfo;
		if (!ai) {
			/* Ran out of addresses to try, so we are
			   done. We should have an error to report. */
			assert(!error_ok(&c->err));
			set_events(f, &c->common, 0);
			simple_socket_wake_all(&c->parent->base);
			break;
		}

		c->next_addrinfo = ai->ai_next;

		/* If we have an existing connecting socket, dispose of it. */
		if (c->common.fd >= 0)
			common_socket_close(&c->common, &c->err);

		error_reset(&c->err);
		c->common.fd = make_socket(ai->ai_family, ai->ai_socktype,
					   &c->err);
		if (c->common.fd < 0)
			continue;

		if (connect(c->common.fd, ai->ai_addr, ai->ai_addrlen) >= 0) {
			/* Immediately connected.  Can this actually
			 * happen?  Handle it anyway. */
			connector_connected(c);
			break;
		}
		else if (blocked()) {
			/* Writeability will indicate that the connection has
			 * been established. */
			set_events(f, &c->common, POLLOUT);
			break;
		}
		else {
			error_errno(&c->err, "connect");
		}
	}
}

static void connector_handle_events(struct socket *gs, short events)
{
	struct connector *c = (struct connector *)gs;
	assert(gs->ops == &connector_common_ops);

	if (events & POLLERR) {
		int e;
		socklen_t len = sizeof e;
		getsockopt(c->common.fd, SOL_SOCKET, SO_ERROR, &e, &len);
		if (e) {
			/* Stash the error and try another address */
			error_errno_val(&c->err, e, "connect");
			connector_connect(c);
			return;
		}
	}

	if (events & POLLOUT) {
		connector_connected(c);
	}
}

static bool_t connector_ok(struct connector *c, struct error *err)
{
	/* We are ok if we have more addrinfos to try, or if there was
	   no error. */
	if (c->next_addrinfo || error_ok(&c->err))
		return 1;

	error_copy(&c->err, err);
	return 0;
}

static struct socket_ops connector_common_ops = {
	connector_handle_events
};

static struct socket_ops client_socket_ops;

static struct client_socket *client_socket_create(
					       struct simple_socket_factory *f,
					       struct connector *c)
{
	struct client_socket *s = xalloc(sizeof *s);
	simple_socket_init(&s->base, &client_socket_ops, f, -1);
	s->connector = c;
	return s;
}

static void client_socket_close(struct socket *gs, struct error *e)
{
	struct client_socket *s = (struct client_socket *)gs;
	assert(gs->ops == &client_socket_ops);

	mutex_lock(&s->base.factory->mutex);

	if (!s->connector)
		simple_socket_close_locked(&s->base, e);
	else
		connector_close(s->connector, e);

	mutex_unlock(&s->base.factory->mutex);
}

static void client_socket_destroy(struct socket *gs)
{
	struct client_socket *s = (struct client_socket *)gs;
	assert(gs->ops == &client_socket_ops);

	mutex_lock(&s->base.factory->mutex);

	if (s->connector)
		connector_destroy(s->connector);

	simple_socket_fini(&s->base);
	mutex_unlock(&s->base.factory->mutex);
	free(s);
}

static ssize_t client_socket_read(struct socket *gs, void *buf, size_t len,
				  struct tasklet *t, struct error *e)
{
	struct client_socket *s = (struct client_socket *)gs;
	ssize_t res;
	assert(gs->ops == &client_socket_ops);

	mutex_lock(&s->base.factory->mutex);

	if (!s->connector) {
		res = simple_socket_read_locked(&s->base, buf, len, t, e);
	}
	else {
		if (connector_ok(s->connector, e))
			simple_socket_blocked_read(&s->base, t);

		res = -1;
	}

	mutex_unlock(&s->base.factory->mutex);
	return res;
}

static ssize_t client_socket_write(struct socket *gs, void *buf, size_t len,
				  struct tasklet *t, struct error *e)
{
	struct client_socket *s = (struct client_socket *)gs;
	ssize_t res;
	assert(gs->ops == &client_socket_ops);

	mutex_lock(&s->base.factory->mutex);

	if (!s->connector) {
		res = simple_socket_write_locked(&s->base, buf, len, t, e);
	}
	else {
		if (connector_ok(s->connector, e))
			simple_socket_blocked_write(&s->base, t);

		res = -1;
	}

	mutex_unlock(&s->base.factory->mutex);
	return res;
}

static struct socket_ops client_socket_ops = {
	simple_socket_handle_events,
	client_socket_read,
	client_socket_write,
	simple_socket_address,
	simple_socket_peer_address,
	client_socket_close,
	simple_socket_partial_close,
	client_socket_destroy
};




struct accepting_socket {
	struct common_socket common;
	struct simple_server_socket *parent;
};

struct simple_server_socket {
	struct server_socket base;
	struct simple_socket_factory *factory;
	struct wait_list accepting;

	/* The accepting sockets */
	struct accepting_socket *sockets;

	unsigned int n_sockets;
};

static struct server_socket_ops simple_server_socket_ops;
static struct socket_ops accepting_socket_ops;

static struct simple_server_socket *simple_server_socket_create(
						struct simple_socket_factory *f,
						int *socket_fds,
						unsigned int n_sockets)
{
	unsigned int i;
	struct simple_server_socket *s;

	s = xalloc(sizeof *s);
	s->base.ops = &simple_server_socket_ops;
	s->factory = f;
	wait_list_init(&s->accepting, 0);
	s->n_sockets = n_sockets;

	/* Wrap the socket fds in listening_socket structs */
	s->sockets = xalloc(n_sockets * sizeof *s->sockets);
	for (i = 0; i < n_sockets; i++) {
		s->sockets[i].parent = s;
		common_socket_init(&s->sockets[i].common,
				   &accepting_socket_ops,
				   socket_fds[i]);
	}

	return s;
}

static struct socket *simple_server_socket_accept(struct server_socket *gs,
						  struct tasklet *t,
						  struct error *e)
{
	int fd;
	unsigned int i;
	struct socket *res = NULL;
	struct simple_server_socket *s = (struct simple_server_socket *)gs;
	assert(gs->ops == &simple_server_socket_ops);

	mutex_lock(&s->factory->mutex);

	/* Go through the accepting sockets for one that yields a
	   connected socket.  This means we do some spurious accept
	   calls.  It would be better to hang a tasklet off each
	   accepting_socket and queue the accepted fds in userspace.
	   But that is a bit more complicated, so this will do for
	   now. */
	for (i = 0; i < s->n_sockets; i++) {
		fd = accept(s->sockets[i].common.fd, 0, 0);
		if (fd >= 0) {
			/* Got one! */
			if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
				error_errno(e, "fcntl");
				close(fd);
			}
			else {
				res = simple_socket_create(s->factory, fd);
			}

			goto out;
		}
		else if (!blocked()) {
			error_errno(e, "accept");
			goto out;
		}
	}

	/* No sockets ready, so wait. */
	for (i = 0; i < s->n_sockets; i++)
		set_events(s->factory, &s->sockets[i].common, POLLIN);

	wait_list_wait(&s->accepting, t);

 out:
	mutex_unlock(&s->factory->mutex);
	return res;
}

static struct sockaddr **simple_server_socket_addresses(
						      struct server_socket *gs,
						      struct error *err)
{
	struct simple_server_socket *s = (struct simple_server_socket *)gs;
	struct sockaddr **addrs;
	unsigned int i;

	assert(gs->ops == &simple_server_socket_ops);

	addrs = xalloc((s->n_sockets + 1) * sizeof *addrs);

	for (i = 0; i < s->n_sockets; i++) {
		struct sockaddr *addr
			= get_socket_address(s->sockets[i].common.fd, err);
		if (!addr)
			goto error;

		addrs[i] = addr;
	}

	addrs[s->n_sockets] = NULL;
	return addrs;

 error:
	while (i-- > 0)
		free(addrs[i]);

	free(addrs);
	return NULL;
}

static void simple_server_socket_close_locked(struct simple_server_socket *s,
					      struct error *e)
{
	unsigned int i;

	for (i = 0; i < s->n_sockets; i++) {
		if (s->sockets[i].common.fd >= 0) {
			set_events(s->factory, &s->sockets[i].common, 0);
			common_socket_close(&s->sockets[i].common, e);
		}
	}
}

static void simple_server_socket_close(struct server_socket *gs,
				       struct error *e)
{
	struct simple_server_socket *s = (struct simple_server_socket *)gs;
	assert(gs->ops == &simple_server_socket_ops);
	mutex_lock(&s->factory->mutex);
	simple_server_socket_close_locked(s, e);
	mutex_unlock(&s->factory->mutex);
}

static void simple_server_socket_destroy(struct server_socket *gs)
{
	unsigned int i;
	struct simple_server_socket *s = (struct simple_server_socket *)gs;
	assert(gs->ops == &simple_server_socket_ops);

	mutex_lock(&s->factory->mutex);

	struct error err;
	error_init(&err);
	simple_server_socket_close_locked(s, &err);
	error_fini(&err);

	for (i = 0; i < s->n_sockets; i++)
		common_socket_fini(&s->sockets[i].common, s->factory);

	wait_list_fini(&s->accepting);
	free(s->sockets);
	mutex_unlock(&s->factory->mutex);
	free(s);
}

static struct server_socket_ops simple_server_socket_ops = {
	simple_server_socket_accept,
	simple_server_socket_addresses,
	simple_server_socket_close,
	simple_server_socket_destroy
};

static void simple_server_socket_handle_events(struct socket *gs, short events)
{
	struct accepting_socket *as = (struct accepting_socket *)gs;
	assert(gs->ops == &accepting_socket_ops);

	wait_list_broadcast(&as->parent->accepting);
	set_events(as->parent->factory, &as->common, 0);
}

static struct socket_ops accepting_socket_ops = {
	simple_server_socket_handle_events
};


static struct socket_factory_ops simple_socket_factory_ops;


static struct socket *connect_address(struct socket_factory *gf,
				      struct sockaddr *sa,
				      struct error *err)
{
	struct simple_socket_factory *f = (struct simple_socket_factory *)gf;
	struct connector *c;
	struct addrinfo ai;

	assert(gf->ops == &simple_socket_factory_ops);

	ai.ai_next = NULL;
	ai.ai_family = sa->sa_family;
	ai.ai_socktype = SOCK_STREAM;
	ai.ai_protocol = 0;
	ai.ai_addr = sa;
	ai.ai_addrlen = sockaddr_len(sa, err);
	if (!ai.ai_addrlen)
		return NULL;

	c = connector_create(f, &ai, NULL);

	mutex_lock(&f->mutex);
	connector_connect(c);
	mutex_unlock(&f->mutex);

	return &c->parent->base.common.base;
}

static struct socket *sf_connect(struct socket_factory *gf,
				 const char *host, const char *service,
				 struct error *err)
{
	/* We should really do the getaddrinfo call asynchronously.
	   glibc has a getaddrinfo_a, but for the sake of portability
	   it might be better to farm it out to another thread. */

	struct simple_socket_factory *f = (struct simple_socket_factory *)gf;
	struct connector *c;
	struct addrinfo hints;
	struct addrinfo *ai;
	int res;

	assert(gf->ops == &simple_socket_factory_ops);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	res = getaddrinfo(host, service, &hints, &ai);
	if (res) {
		error_set(err, ERROR_OS, "resolving network address: %s",
			  gai_strerror(res));
		return NULL;
	}

	c = connector_create(f, ai, ai);

	mutex_lock(&f->mutex);
	connector_connect(c);
	mutex_unlock(&f->mutex);

	return &c->parent->base.common.base;
}

static int make_listening_socket(int family, int socktype, struct error *err)
{
	int fd = make_socket(family, socktype, err);

	if (fd >= 0 && listen(fd, SOMAXCONN) < 0) {
		error_errno(err, "listen");
		close(fd);
		fd = -1;
	}

	return fd;
}

static struct server_socket *unbound_server_socket(struct socket_factory *gf,
						   struct error *e)
{
	struct simple_socket_factory *f = (struct simple_socket_factory *)gf;
	int fd;
	struct simple_server_socket *s;

	assert(gf->ops == &simple_socket_factory_ops);

	fd = make_listening_socket(AF_INET, SOCK_STREAM, e);
	if (fd < 0)
		return NULL;

	s = simple_server_socket_create(f, &fd, 1);
	return &s->base;
}

static int make_bound_socket(struct addrinfo *ai, struct error *err)
{
	int on = 1;
	const char *op;
	int fd = make_socket(ai->ai_family, ai->ai_socktype, err);
	if (fd < 0)
		goto out;

	op = "setsockopt";
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) < 0)
		goto error;

	/* disable dual-binding in order to bind to the same port on
	   IPv4 and IPv6. */
	if (ai->ai_family == AF_INET6
	    && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof on) < 0)
		goto error;

	op = "bind";
	if (bind(fd, ai->ai_addr, ai->ai_addrlen) < 0)
		goto error;

	op = "listen";
	if (listen(fd, SOMAXCONN) < 0)
	    goto error;

	return fd;

 error:
	error_errno(err, op);
	close(fd);
 out:
	return -1;
}

static struct server_socket *bound_server_socket(struct socket_factory *gf,
						 const char *host,
						 const char *service,
						 struct error *err_out)
{
	struct simple_socket_factory *f = (struct simple_socket_factory *)gf;
	struct addrinfo *ai, *addrinfos, hints;
	unsigned int n_addrs = 0, n_sockets = 0;
	int *socket_fds;
	int res;
	struct simple_server_socket *s;
	struct error err;

	assert(gf->ops == &simple_socket_factory_ops);

	// See RFC4038, section 6.3.1.

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	res = getaddrinfo(host, service, &hints, &addrinfos);
	if (res) {
		error_set(err_out, ERROR_OS, "resolving network address: %s",
			  gai_strerror(res));
		return NULL;
	}

	/* Should have at least one addrinfo */
	assert(addrinfos);

	/* Count how many addresses we have */
	for (ai = addrinfos; ai; ai = ai->ai_next)
		n_addrs++;

	/* Allocate space for the fds */
	socket_fds = xalloc(n_addrs * sizeof *socket_fds);

	/* Create sockets */
	error_init(&err);
	for (ai = addrinfos; ai; ai = ai->ai_next) {
		int fd = make_bound_socket(ai, &err);
		if (fd >= 0)
			socket_fds[n_sockets++] = fd;
	}

	freeaddrinfo(addrinfos);

	/* If no sockets were successfully bound, report an error. */
	if (n_sockets == 0) {
		error_propogate(&err, err_out);
		free(socket_fds);
		return NULL;
	}

	s = simple_server_socket_create(f, socket_fds, n_sockets);
	error_fini(&err);
	free(socket_fds);
	return &s->base;
}

static void simple_socket_factory_poll(struct socket_factory *gf,
				       struct error *e)
{
	struct simple_socket_factory *f = (struct simple_socket_factory *)gf;
	struct pollfd *pollfds;
	int pollfds_used, i;

	mutex_lock(&f->mutex);
	apply_updates(f);
	pollfds = f->pollfds;
	pollfds_used = f->pollfds_used;
	mutex_unlock(&f->mutex);

	if (poll(pollfds, pollfds_used, -1) < 0) {
		if (errno != EINTR)
			error_errno(e, "poll");

		return;
	}

	mutex_lock(&f->mutex);

	for (i = 0; i < pollfds_used; i++) {
		if (pollfds[i].revents) {
			struct common_socket *s = f->pollfd_sockets[i];
			if (s)
				s->base.ops->handle_events(&s->base,
							   pollfds[i].revents);
		}
	}

	mutex_unlock(&f->mutex);
}


static struct socket_factory_ops simple_socket_factory_ops = {
	sf_connect,
	connect_address,
	unbound_server_socket,
	bound_server_socket,
	simple_socket_factory_poll
};

static struct socket_factory *default_socket_factory;

static void socket_factory_cleanup(void)
{
	struct socket_factory *sf = default_socket_factory;
	if (sf)
		socket_factory_destroy(sf);
}

struct socket_factory *socket_factory()
{
	for (;;) {
		struct socket_factory *sf = default_socket_factory;
		if (sf)
			return sf;

		sf = socket_factory_create();
		if (__sync_bool_compare_and_swap(&default_socket_factory, NULL,
						 sf)) {
			atexit(socket_factory_cleanup);
			return sf;
		}

		socket_factory_destroy(sf);
	}
}

static bool_t sf_stop_run;

void sigint_handler(int sig)
{
	sf_stop_run = 1;
}

void socket_factory_run(struct socket_factory *f, struct error *e)
{
	sighandler_t old_sigint;

	sf_stop_run = 0;
	old_sigint = signal(SIGINT, sigint_handler);

	while (!sf_stop_run && error_ok(e)) {
		run_queue_thread_run();

		if (sf_stop_run)
			break;

		socket_factory_poll(f, e);
	}

	signal(SIGINT, old_sigint);
}

void socket_factory_stop(void)
{
	sf_stop_run = 1;
}
