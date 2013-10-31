#ifndef MOLERAT_SOCKET_H
#define MOLERAT_SOCKET_H

#include "base.h"

struct tasklet;
struct sockaddr;

struct socket {
	struct socket_ops *ops;
};

struct socket_ops {
	ssize_t (*read)(struct socket *s, void *buf, size_t len,
			struct tasklet *t, struct error *e);
	ssize_t (*write)(struct socket *s, void *buf,  size_t len,
			 struct tasklet *t, struct error *e);
	struct sockaddr *(*address)(struct socket *s, struct error *e);
	struct sockaddr *(*peer_address)(struct socket *s, struct error *e);
	void (*close)(struct socket *s, struct error *e);
	void (*partial_close)(struct socket *s, bool_t writes, bool_t reads,
			      struct error *e);
	void (*destroy)(struct socket *s);
};

static inline ssize_t socket_read(struct socket *s, void *buf, size_t len,
				  struct tasklet *t, struct error *e)
{
	return s->ops->read(s, buf, len, t, e);
}

static inline ssize_t socket_write(struct socket *s, void *buf, size_t len,
				   struct tasklet *t, struct error *e)
{
	return s->ops->write(s, buf, len, t, e);
}

static inline struct sockaddr *socket_address(struct socket *s, struct error *e)
{
	return s->ops->address(s, e);
}

static inline struct sockaddr *socket_peer_address(struct socket *s,
						   struct error *e)
{
	return s->ops->peer_address(s, e);
}

static inline void socket_close(struct socket *s, struct error *e)
{
	return s->ops->close(s, e);
}

static inline void socket_partial_close(struct socket *s,
					bool_t writes, bool_t reads,
					struct error *e)
{
	return s->ops->partial_close(s, writes, reads, e);
}

static inline void socket_destroy(struct socket *s)
{
	return s->ops->destroy(s);
}


struct server_socket {
	struct server_socket_ops *ops;
};

struct server_socket_ops {
	struct socket *(*accept)(struct server_socket *s,
				 struct tasklet *t, struct error *e);
	struct sockaddr **(*addresses)(struct server_socket *s,
				       struct error *e);
	void (*close)(struct server_socket *s, struct error *e);
	void (*destroy)(struct server_socket *s);
};

static inline struct socket *server_socket_accept(struct server_socket *s,
						  struct tasklet *t,
						  struct error *e)
{
	return s->ops->accept(s, t, e);
}

static inline struct sockaddr **server_socket_addresses(struct server_socket *s,
							struct error *e)
{
	return s->ops->addresses(s, e);
}

static inline void server_socket_close(struct server_socket *s,
				       struct error *e)
{
	return s->ops->close(s, e);
}

static inline void server_socket_destroy(struct server_socket *s)
{
	return s->ops->destroy(s);
}


struct socket_factory {
	struct socket_factory_ops *ops;
};

struct socket_factory_ops {
	struct socket *(*connect)(struct socket_factory *f,
				  const char *host, const char *service,
				  struct error *e);
	struct socket *(*connect_address)(struct socket_factory *f,
					  struct sockaddr *sa,
					  struct error *e);
	struct server_socket *(*unbound_server_socket)(struct socket_factory *f,
						       struct error *e);
	struct server_socket *(*bound_server_socket)(struct socket_factory *f,
						     const char *host,
						     const char *service,
						     struct error *e);
};

static inline struct socket *socket_factory_connect(struct socket_factory *f,
						    const char *host,
						    const char *service,
						    struct error *e)
{
	return f->ops->connect(f, host, service, e);
}

static inline struct socket *socket_factory_connect_address(
		 struct socket_factory *f, struct sockaddr *sa, struct error *e)
{
	return f->ops->connect_address(f, sa, e);
}

static inline struct server_socket *socket_factory_unbound_server_socket(
				     struct socket_factory *f, struct error *e)
{
	return f->ops->unbound_server_socket(f, e);
}

static inline struct server_socket *socket_factory_bound_server_socket(
						       struct socket_factory *f,
						       const char *host,
						       const char *service,
						       struct error *e)
{
	return f->ops->bound_server_socket(f, host, service, e);
}


struct socket_factory *socket_factory();

void socket_factory_run(struct error *e);
void socket_factory_stop(void);

char *print_sockaddr(struct sockaddr *sa, struct error *err);
void free_sockaddrs(struct sockaddr **addrs);

#endif
