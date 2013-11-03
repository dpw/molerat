#ifndef MOLERAT_HTTP_SERVER_H
#define MOLERAT_HTTP_SERVER_H

struct http_server *http_server_create(struct server_socket *s,
				       bool_t verbose);
void http_server_destroy(struct http_server *hs);

#endif
