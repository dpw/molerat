#ifndef MOLERAT_SOCKET_H
#define MOLERAT_SOCKET_H

#include "base.h"
#include "stream.h"

struct sockaddr;

struct socket {
	struct socket_ops *ops;
};

static inline struct stream *socket_stream(struct socket *s)
{
	return (struct stream *)s;
}

struct socket_ops {
	struct stream_ops stream_ops;
	void (*partial_close)(struct socket *s, bool_t writes, bool_t reads,
			      struct error *e);
	struct sockaddr *(*address)(struct socket *s, struct error *e);
	struct sockaddr *(*peer_address)(struct socket *s, struct error *e);
};

static inline void socket_destroy(struct socket *s)
{
	stream_destroy(socket_stream(s));
}

static inline ssize_t socket_read(struct socket *s, void *buf, size_t len,
				  struct tasklet *t, struct error *e)
{
	return stream_read(socket_stream(s), buf, len, t, e);
}

static inline ssize_t socket_write(struct socket *s, const void *buf,
				   size_t len, struct tasklet *t,
				   struct error *e)
{
	return stream_write(socket_stream(s), buf, len, t, e);
}

static inline void socket_close(struct socket *s, struct error *e)
{
	stream_close(socket_stream(s), e);
}

static inline void socket_partial_close(struct socket *s,
					bool_t writes, bool_t reads,
					struct error *e)
{
	return s->ops->partial_close(s, writes, reads, e);
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

char *print_sockaddr(struct sockaddr *sa, struct error *err);
void free_sockaddrs(struct sockaddr **addrs);

#endif
