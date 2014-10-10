#ifndef MOLERAT_HTTP_SERVER_H
#define MOLERAT_HTTP_SERVER_H

#include <molerat/http_reader.h>
#include <molerat/http_writer.h>

struct http_server_exchange;
void http_server_exchange_done(struct http_server_exchange *ex,
			       struct error *err);

typedef void (*http_server_callback)(struct http_server_exchange *ex,
				     struct http_reader *hr,
				     struct http_writer *hw);

struct http_server *http_server_create(struct server_socket *s,
				       http_server_callback cb);
void http_server_destroy(struct http_server *hs);



#endif
