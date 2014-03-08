#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <molerat/socket.h>
#include <molerat/tasklet.h>
#include <molerat/watched_fd.h>

static int would_block(void)
{
	switch (errno) {
		/* POSIX says EWOULDBLOCK can be distinct from EAGAIN */
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


struct simple_socket {
	struct socket base;
	struct mutex mutex;
	struct wait_list reading;
	struct wait_list writing;
	struct watched_fd *watched_fd;
	int fd;
};

static struct socket_ops simple_socket_ops;

static void simple_socket_handle_events(void *v_s, poll_events_t events)
{
	struct simple_socket *s = v_s;

	if (events & (WATCHED_FD_IN | WATCHED_FD_ERR))
		wait_list_broadcast(&s->reading);

	if (events & (WATCHED_FD_OUT | WATCHED_FD_ERR))
		wait_list_broadcast(&s->writing);
}

static void simple_socket_init(struct simple_socket *s,
			       struct socket_ops *ops, int fd)
{
	s->base.ops = ops;
	mutex_init(&s->mutex);
	wait_list_init(&s->reading, 0);
	wait_list_init(&s->writing, 0);

	s->fd = fd;
	if (fd >= 0)
		s->watched_fd
			= watched_fd_create(fd, simple_socket_handle_events, s);
}

static struct socket *simple_socket_create(int fd)
{
	struct simple_socket *s = xalloc(sizeof *s);
	simple_socket_init(s, &simple_socket_ops, fd);
	return &s->base;
}

static enum stream_result simple_socket_close_locked(struct simple_socket *s,
						     struct error *e)
{
	if (s->fd >= 0) {
		watched_fd_destroy(s->watched_fd);
		if (close(s->fd) < 0 && e) {
			error_errno(e, "close");
			return STREAM_ERROR;
		}

		s->fd = -1;
	}

	return STREAM_OK;
}

static void simple_socket_fini(struct simple_socket *s)
{
	simple_socket_close_locked(s, NULL);
	wait_list_fini(&s->reading);
	wait_list_fini(&s->writing);
	mutex_unlock_fini(&s->mutex);
}

static void simple_socket_destroy(struct stream *gs)
{
	struct simple_socket *s = (struct simple_socket *)gs;
	assert(((struct socket *)gs)->ops == &simple_socket_ops);
	mutex_lock(&s->mutex);
	simple_socket_fini(s);
	free(s);
}

static enum stream_result simple_socket_close(struct stream *gs,
					      struct tasklet *t,
					      struct error *e)
{
	struct simple_socket *s = (struct simple_socket *)gs;
	bool_t res;

	(void)t;

	assert(((struct socket *)gs)->ops == &simple_socket_ops);
	mutex_lock(&s->mutex);
	res = simple_socket_close_locked(s, e);
	mutex_unlock(&s->mutex);

	return res;
}

static void simple_socket_wake_all(struct simple_socket *s)
{
	wait_list_broadcast(&s->reading);
	wait_list_broadcast(&s->writing);
}

static void simple_socket_set_fd(struct simple_socket *s, int fd,
				 struct watched_fd *watched_fd)
{
	assert(s->fd < 0);
	assert(fd >= 0);

	s->fd = fd;
	s->watched_fd = watched_fd;
	watched_fd_set_handler(s->watched_fd, simple_socket_handle_events, s);
	simple_socket_wake_all(s);
}

static struct sockaddr *simple_socket_address(struct socket *gs,
					      struct error *err)
{
	struct simple_socket *s = (struct simple_socket *)gs;
	return get_socket_address(s->fd, err);
}

static struct sockaddr *simple_socket_peer_address(struct socket *gs,
						   struct error *err)
{
	struct simple_socket *s = (struct simple_socket *)gs;
	return get_socket_peer_address(s->fd, err);
}

static ssize_t simple_socket_read(struct stream *gs, void *buf, size_t len,
				  struct tasklet *t, struct error *e)
{
	struct simple_socket *s = (struct simple_socket *)gs;
	ssize_t res;

	assert(((struct socket *)gs)->ops == &simple_socket_ops);
	mutex_lock(&s->mutex);

	if (s->fd >= 0) {
		res = read(s->fd, buf, len);
		if (likely(res > 0))
			goto out;

		if (res == 0) {
			res = len != 0 ? STREAM_END : 0;
			goto out;
		}

		if (would_block()) {
			wait_list_wait(&s->reading, t);
			if (!watched_fd_set_interest(s->watched_fd,
						     WATCHED_FD_IN, e))
				goto error;

			res = STREAM_WAITING;
			goto out;
		}

		error_errno(e, "read");
	}
	else {
		error_set(e, ERROR_INVALID, "socket_read: closed socket");
	}

 error:
	res = STREAM_ERROR;
 out:
	mutex_unlock(&s->mutex);
	return res;
}

static ssize_t simple_socket_write(struct stream *gs, const void *buf,
                                  size_t len, struct tasklet *t,
                                  struct error *e)
{
       struct simple_socket *s = (struct simple_socket *)gs;
       ssize_t res;

       assert(((struct socket *)gs)->ops == &simple_socket_ops);
       mutex_lock(&s->mutex);

       if (s->fd >= 0) {
               res = write(s->fd, buf, len);
               if (likely(res >= 0))
                       goto out;

               if (would_block()) {
                       wait_list_wait(&s->writing, t);
			if (!watched_fd_set_interest(s->watched_fd,
						     WATCHED_FD_OUT, e))
				goto error;

                       res = STREAM_WAITING;
                       goto out;
               }

               error_errno(e, "write");
       }
       else {
               error_set(e, ERROR_INVALID, "socket_write: closed socket");
       }

 error:
       res = STREAM_ERROR;
 out:
       mutex_unlock(&s->mutex);
       return res;
}

static bool_t simple_socket_partial_close(struct socket *gs,
					  bool_t read_else_write,
					  struct error *e)
{
	struct simple_socket *s = (struct simple_socket *)gs;
	bool_t res = TRUE;

	mutex_lock(&s->mutex);

	if (shutdown(s->fd, read_else_write ? SHUT_RD : SHUT_WR) < 0) {
		error_errno(e, "shutdown");
		res = FALSE;
	}

	mutex_unlock(&s->mutex);
	return res;
}

static struct socket_ops simple_socket_ops = {
	{
		simple_socket_destroy,
		simple_socket_read,
		simple_socket_write,
		simple_socket_close,
	},
	simple_socket_partial_close,
	simple_socket_address,
	simple_socket_peer_address,
};


/* The connector is responsible for trying addresses until it
   successfully opens a socket, and then supplying it to the
   client_socket. */
struct connector {
	struct client_socket *socket;

	struct tasklet tasklet;
	struct wait_list connecting;
	struct watched_fd *watched_fd;
	int fd;
	int connected;
	struct addrinfo *next_addrinfo;
	struct addrinfo *addrinfos;
	void (*fai)(struct addrinfo *s); /* the freeaddrinfo function to use */
	struct error err;
};

struct client_socket {
	struct simple_socket base;
	struct connector *connector;
};

static struct socket_ops client_socket_ops;

static void connector_handle_events(void *v_c, poll_events_t events);
static void start_connecting(struct connector *c);
static void finish_connecting(void *v_c);

static struct client_socket *client_socket_create(
					      struct addrinfo *addrinfos,
					      void (*fai)(struct addrinfo *))
{
	struct connector *c = xalloc(sizeof *c);
	struct client_socket *s = xalloc(sizeof *s);

	c->socket = s;

	tasklet_init(&c->tasklet, &s->base.mutex, c);
	wait_list_init(&c->connecting, 0);
	c->fd = -1;
	c->connected = 0;
	c->next_addrinfo = c->addrinfos = addrinfos;
	c->fai = fai;
	error_init(&c->err);

	simple_socket_init(&s->base, &client_socket_ops, -1);
	s->connector = c;

	/* Start connecting */
	mutex_lock(&s->base.mutex);
	start_connecting(c);
	tasklet_later(&c->tasklet, finish_connecting);
	mutex_unlock(&s->base.mutex);

	return s;
}

static void connector_close(struct connector *c)
{
	if (c->fd >= 0) {
		watched_fd_destroy(c->watched_fd);
		close(c->fd);
		c->fd = -1;
	}
}

static void connector_destroy(struct connector *c)
{
	connector_close(c);
	wait_list_fini(&c->connecting);
	tasklet_fini(&c->tasklet);
	error_fini(&c->err);
	c->fai(c->addrinfos);
	free(c);
}

static void start_connecting(struct connector *c)
{
	for (;;) {
		struct addrinfo *ai = c->next_addrinfo;
		if (!ai)
			break;

		/* If we have an existing connecting socket, dispose of it. */
		connector_close(c);

		c->next_addrinfo = ai->ai_next;
		error_reset(&c->err);
		c->fd = make_socket(ai->ai_family, ai->ai_socktype, &c->err);
		if (c->fd < 0)
			continue;

		c->watched_fd
			= watched_fd_create(c->fd, connector_handle_events, c);
		if (connect(c->fd, ai->ai_addr, ai->ai_addrlen) >= 0) {
			/* Immediately connected.  Not sure this can
			   actually happen. */
			wait_list_up(&c->connecting, 1);
			return;
		}
		else if (would_block()) {
			/* Writeability will indicate that the connection has
			 * been established. */
			if (!watched_fd_set_interest(c->watched_fd,
						     WATCHED_FD_OUT, &c->err))
				/* Give up and propagate the error */
				break;

			return;
		}
		else {
			error_errno(&c->err, "connect");
		}
	}

	/* Ran out of addresses to try, so we are done. We should have
	   an error to report. */
	assert(!error_ok(&c->err));
	simple_socket_wake_all(&c->socket->base);
}

static void finish_connecting(void *v_c)
{
	struct connector *c = v_c;
	struct client_socket *s = c->socket;

	for (;;) {
		if (!wait_list_down(&c->connecting, 1, &c->tasklet))
			return;

		if (c->connected) {
			int fd = c->fd;
			struct watched_fd *watched_fd = c->watched_fd;
			c->fd = -1;

			connector_destroy(c);
			s->connector = NULL;

			simple_socket_set_fd(&s->base, fd, watched_fd);

			/* Access to the ops pointer is not locked.
			 * But it is fine if some threads continue to
			 * use the old client_socket_ops value. */
			s->base.base.ops = &simple_socket_ops;

			/* Tasklet got destroyed, so we are still
			   holding the associated lock. */
			mutex_unlock(&s->base.mutex);
			return;
		}
		else {
			/* Got POLLERR */
			int e;
			socklen_t len = sizeof e;
			const char *syscall = "connect";

			if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &e, &len)) {
				e = errno;
				syscall = "getsockopt";
			}

			if (e) {
				/* Stash the error and try another address */
				error_errno_val(&c->err, e, "%s", syscall);
				start_connecting(c);
			}
			/* Strange, no error.  Continue to poll. */
			else if (!watched_fd_set_interest(c->watched_fd,
						  WATCHED_FD_OUT, &c->err)) {
				simple_socket_wake_all(&c->socket->base);
			}
		}
	}
}

static void connector_handle_events(void *v_c, poll_events_t events)
{
	struct connector *c = v_c;

	if (events & (WATCHED_FD_OUT | WATCHED_FD_ERR)) {
		c->connected = (events == WATCHED_FD_OUT);
		wait_list_up(&c->connecting, 1);
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

static enum stream_result client_socket_close(struct stream *gs,
					      struct tasklet *t,
					      struct error *e)
{
	struct client_socket *s = (struct client_socket *)gs;

	assert(((struct socket *)gs)->ops == &client_socket_ops);
	mutex_lock(&s->base.mutex);

	if (!s->connector) {
		mutex_unlock(&s->base.mutex);
		return simple_socket_close(gs, t, e);
	}
	else {
		connector_destroy(s->connector);
		s->connector = NULL;
		mutex_unlock(&s->base.mutex);
		return STREAM_OK;
	}
}

static void client_socket_destroy(struct stream *gs)
{
	struct client_socket *s = (struct client_socket *)gs;
	assert(((struct socket *)gs)->ops == &client_socket_ops);

	mutex_lock(&s->base.mutex);

	if (s->connector)
		connector_destroy(s->connector);

	simple_socket_fini(&s->base);
	free(s);
}

static ssize_t client_socket_read(struct stream *gs, void *buf, size_t len,
				  struct tasklet *t, struct error *e)
{
	struct client_socket *s = (struct client_socket *)gs;
	ssize_t res;

	mutex_lock(&s->base.mutex);

	if (!s->connector) {
		mutex_unlock(&s->base.mutex);
		return simple_socket_read(gs, buf, len, t, e);
	}
	else if (connector_ok(s->connector, e)) {
		wait_list_wait(&s->base.reading, t);
		res = STREAM_WAITING;
	}
	else {
		res = STREAM_ERROR;
	}

	mutex_unlock(&s->base.mutex);
	return res;
}

static ssize_t client_socket_write(struct stream *gs, const void *buf,
				   size_t len, struct tasklet *t,
				   struct error *e)
{
	struct client_socket *s = (struct client_socket *)gs;
	ssize_t res;

	mutex_lock(&s->base.mutex);

	if (!s->connector) {
		mutex_unlock(&s->base.mutex);
		return simple_socket_write(gs, buf, len, t, e);
	}
	else if (connector_ok(s->connector, e)) {
		wait_list_wait(&s->base.writing, t);
		res = STREAM_WAITING;
	}
	else {
		res = STREAM_ERROR;
	}

	mutex_unlock(&s->base.mutex);
	return res;
}

static struct socket_ops client_socket_ops = {
	{
		client_socket_destroy,
		client_socket_read,
		client_socket_write,
		client_socket_close,
	},
	simple_socket_partial_close,
	simple_socket_address,
	simple_socket_peer_address,
};


struct simple_server_socket {
	struct server_socket base;
	struct mutex mutex;
	struct wait_list accepting;

	/* The accepting sockets */
	struct server_fd *fds;
	int n_fds;
};

struct server_fd {
	struct simple_server_socket *socket;
	struct watched_fd *watched_fd;
	int fd;
	bool_t ready;
};


static struct server_socket_ops simple_server_socket_ops;

static void accept_handle_events(void *v_sfd, poll_events_t events)
{
	struct server_fd *sfd = v_sfd;
	(void)events;
	sfd->ready = TRUE;
	wait_list_up(&sfd->socket->accepting, 1);
}

static struct simple_server_socket *simple_server_socket_create(int *fds,
								int n_fds)
{
	int i;

	struct simple_server_socket *s = xalloc(sizeof *s);
	s->base.ops = &simple_server_socket_ops;
	mutex_init(&s->mutex);
	wait_list_init(&s->accepting, 0);
	s->n_fds = n_fds;

	s->fds = xalloc(n_fds * sizeof *s->fds);
	for (i = 0; i < n_fds; i++) {
		struct server_fd *sfd = &s->fds[i];
		sfd->socket = s;
		sfd->fd = fds[i];
		sfd->ready = FALSE;
		sfd->watched_fd
			= watched_fd_create(fds[i], accept_handle_events, sfd);
	}

	return s;
}

static void simple_server_socket_close_locked(struct simple_server_socket *s,
					      struct error *e)
{
	int i;

	for (i = 0; i < s->n_fds; i++) {
		int fd = s->fds[i].fd;
		if (fd >= 0) {
			watched_fd_destroy(s->fds[i].watched_fd);
			if (close(fd) < 0 && e)
				error_errno(e, "close");
		}
	}

	free(s->fds);
	s->fds = NULL;
}

static void simple_server_socket_close(struct server_socket *gs,
				       struct error *e)
{
	struct simple_server_socket *s = (struct simple_server_socket *)gs;
	assert(gs->ops == &simple_server_socket_ops);
	mutex_lock(&s->mutex);
	simple_server_socket_close_locked(s, e);
	mutex_unlock(&s->mutex);
}

static void simple_server_socket_destroy(struct server_socket *gs)
{
	struct simple_server_socket *s = (struct simple_server_socket *)gs;
	assert(gs->ops == &simple_server_socket_ops);

	mutex_lock(&s->mutex);
	simple_server_socket_close_locked(s, NULL);
	wait_list_fini(&s->accepting);
	mutex_unlock_fini(&s->mutex);
	free(s);
}

static struct socket *simple_server_socket_accept(struct server_socket *gs,
						  struct tasklet *t,
						  struct error *e)
{
	int fd;
	int i;
	struct socket *res = NULL;
	struct simple_server_socket *s = (struct simple_server_socket *)gs;
	assert(gs->ops == &simple_server_socket_ops);

	mutex_lock(&s->mutex);

	do {
		for (i = 0; i < s->n_fds; i++) {
			struct server_fd *sfd = &s->fds[i];

			if (!sfd->ready)
				continue;

			/* Clear sfd->ready before trying accept, to
			   avoid a race with accept_handle_events. */
			sfd->ready = FALSE;
			fd = accept(sfd->fd, NULL, NULL);
			if (fd >= 0) {
				/* Got one! But there might be more to come. */
				sfd->ready = TRUE;

				if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
					error_errno(e, "fcntl");
					close(fd);
				}
				else {
					res = simple_socket_create(fd);
				}

				goto out;
			}
			else if (!would_block()) {
				error_errno(e, "accept");
				goto out;
			}
		}

		/* No sockets ready, so wait. */
		for (i = 0; i < s->n_fds; i++)
			if (!watched_fd_set_interest(s->fds[i].watched_fd,
						     WATCHED_FD_IN, e))
				goto out;
	} while (wait_list_down(&s->accepting, 1, t));

 out:
	mutex_unlock(&s->mutex);
	return res;
}

static struct sockaddr **simple_server_socket_addresses(
						      struct server_socket *gs,
						      struct error *err)
{
	struct simple_server_socket *s = (struct simple_server_socket *)gs;
	struct sockaddr **addrs;
	int i;

	assert(gs->ops == &simple_server_socket_ops);
	mutex_lock(&s->mutex);

	addrs = xalloc((s->n_fds + 1) * sizeof *addrs);

	for (i = 0; i < s->n_fds; i++) {
		struct sockaddr *addr = get_socket_address(s->fds[i].fd, err);
		if (!addr)
			goto error;

		addrs[i] = addr;
	}

	mutex_unlock(&s->mutex);
	addrs[s->n_fds] = NULL;
	return addrs;

 error:
	mutex_unlock(&s->mutex);

	while (i-- > 0)
		free(addrs[i]);

	free(addrs);
	return NULL;
}

static struct server_socket_ops simple_server_socket_ops = {
	simple_server_socket_accept,
	simple_server_socket_addresses,
	simple_server_socket_close,
	simple_server_socket_destroy
};



static struct socket_factory_ops simple_socket_factory_ops;

static void our_freeaddrinfo(struct addrinfo *ai)
{
	while (ai) {
		struct addrinfo *next = ai->ai_next;
		free(ai->ai_addr);
		free(ai);
		ai = next;
	}
}

static struct socket *connect_addresses(struct socket_factory *gf,
					struct sockaddr **addrs,
					struct error *err)
{
	struct sockaddr *addr;
	struct addrinfo *ai;
	struct addrinfo *addrinfos;
	struct addrinfo **next = &addrinfos;

	assert(gf->ops == &simple_socket_factory_ops);

	while ((addr = *addrs++)) {
		ai = xalloc(sizeof *ai);
		ai->ai_next = NULL;
		ai->ai_family = addr->sa_family;
		ai->ai_socktype = SOCK_STREAM;
		ai->ai_protocol = 0;
		ai->ai_addr = NULL;
		ai->ai_addrlen = sockaddr_len(addr, err);

		*next = ai;
		next = &ai->ai_next;

		if (!ai->ai_addrlen)
			goto error;

		ai->ai_addr = xalloc(ai->ai_addrlen);
		memcpy(ai->ai_addr, addr, ai->ai_addrlen);
	}

	if (addrinfos)
		return &client_socket_create(addrinfos,
					     our_freeaddrinfo)->base.base;

	error_set(err, ERROR_INVALID, "no addresses to connect to");
	return NULL;

 error:
	our_freeaddrinfo(addrinfos);
	return NULL;
}

static struct socket *sf_connect(struct socket_factory *gf,
				 const char *host, const char *service,
				 struct error *err)
{
	/* We should really do the getaddrinfo call asynchronously.
	   glibc has a getaddrinfo_a, but for the sake of portability
	   it might be better to farm it out to another thread. */

	struct addrinfo hints;
	struct addrinfo *ai;
	int res;

	assert(gf->ops == &simple_socket_factory_ops);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	res = getaddrinfo(host, service, &hints, &ai);
	if (!res)
		/* SuS says "The list shall include at least one
		 * addrinfo structure", so no need to check ai */
		return &client_socket_create(ai, freeaddrinfo)->base.base;

	error_set(err, ERROR_OS, "resolving network address %s:%s: %s",
		  host, service, gai_strerror(res));
	return NULL;
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
	int fd;
	struct simple_server_socket *s;

	assert(gf->ops == &simple_socket_factory_ops);

	fd = make_listening_socket(AF_INET, SOCK_STREAM, e);
	if (fd < 0)
		return NULL;

	s = simple_server_socket_create(&fd, 1);
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
	error_errno(err, "%s", op);
	close(fd);
 out:
	return -1;
}

static struct server_socket *bound_server_socket(struct socket_factory *gf,
						 const char *host,
						 const char *service,
						 struct error *err_out)
{
	struct addrinfo *ai, *addrinfos, hints;
	int n_addrs = 0, n_sockets = 0;
	int *fds;
	int res;
	struct simple_server_socket *s;
	struct error err;

	assert(gf->ops == &simple_socket_factory_ops);

	/* See RFC4038, section 6.3.1. */

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
	fds = xalloc(n_addrs * sizeof *fds);

	/* Create sockets */
	error_init(&err);
	for (ai = addrinfos; ai; ai = ai->ai_next) {
		int fd = make_bound_socket(ai, &err);
		if (fd >= 0)
			fds[n_sockets++] = fd;
	}

	freeaddrinfo(addrinfos);

	/* If no sockets were successfully bound, report an error. */
	if (n_sockets == 0) {
		error_propogate(&err, err_out);
		free(fds);
		return NULL;
	}

	s = simple_server_socket_create(fds, n_sockets);
	error_fini(&err);
	free(fds);
	return &s->base;
}

static struct socket_factory_ops simple_socket_factory_ops = {
	sf_connect,
	connect_addresses,
	unbound_server_socket,
	bound_server_socket
};

static struct socket_factory simple_socket_factory = {
	&simple_socket_factory_ops
};

struct socket_factory *socket_factory()
{
	return &simple_socket_factory;
}
