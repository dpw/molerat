#ifndef MOLERAT_ECHO_SERVER_H
#define MOLERAT_ECHO_SERVER_H

struct echo_server *echo_server_create(struct server_socket *s,
				       bool_t verbose, struct error *err);
void echo_server_destroy(struct echo_server *es);
struct sockaddr **echo_server_addresses(struct echo_server *es,
					struct error *err);

#endif
