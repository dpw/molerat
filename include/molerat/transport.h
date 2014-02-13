#ifndef MOLERAT_TRANSPORT_H
#define MOLERAT_TRANSPORT_H

#include <molerat/base.h>

struct stream;

struct address {
	struct address_ops *ops;
};

struct address_ops {
	void (*release)(struct address *a);
};

static inline void address_release(struct address *a)
{
	a->ops->release(a);
}


struct async_server {
	struct async_server_ops *ops;
};

struct async_server_ops {
	void (*destroy)(struct async_server *s);
	struct address *(*get_address)(struct async_server *s,
				       struct error *err);
};

static inline void async_server_destroy(struct async_server *s)
{
	s->ops->destroy(s);
}

static inline struct address *async_server_address(struct async_server *s,
						   struct error *err)
{
	return s->ops->get_address(s, err);
}


typedef void (*async_message_handler_t)(struct stream *input, void *data);

struct async_transport {
	struct async_transport_ops *ops;
};

struct async_transport_ops {
	void (*destroy)(struct async_transport *t);
	struct async_server *(*serve)(struct async_transport *t,
				      async_message_handler_t handler,
				      void *data, struct error *err);
	struct stream *(*send)(struct async_transport *t, struct address *addr,
			       struct error *err);
};

static inline void async_transport_destroy(struct async_transport *t)
{
	t->ops->destroy(t);
}

static inline struct async_server *async_transport_serve(
					      struct async_transport *t,
					      async_message_handler_t handler,
					      void *data, struct error *err)
{
	return t->ops->serve(t, handler, data, err);
}

static inline struct stream *async_transport_send(struct async_transport *t,
						  struct address *addr,
						  struct error *err)
{
	return t->ops->send(t, addr, err);
}

#endif
