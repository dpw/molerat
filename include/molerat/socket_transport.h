#ifndef MOLERAT_SOCKET_TRANSPORT_H
#define MOLERAT_SOCKET_TRANSPORT_H

#include <molerat/transport.h>

struct socket_factory;

struct async_transport *socket_transport_create(struct socket_factory *sf,
						const char *bind_host);

#endif
