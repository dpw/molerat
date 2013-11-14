#ifndef MOLERAT_HTTP_WRITER_H
#define MOLERAT_HTTP_WRITER_H

#include "buffer.h"

struct socket;
struct tasklet;
struct error;

struct http_writer {
	enum {
		HTTP_WRITER_INIT,
		HTTP_WRITER_HEADERS,
		HTTP_WRITER_PREBODY,
		HTTP_WRITER_BODY
	} state;
	struct growbuf prebody;
	struct drainbuf prebody_out;
	struct socket *socket;
};

void http_writer_init(struct http_writer *w, struct socket *socket);
void http_writer_fini(struct http_writer *w);

void http_writer_request(struct http_writer *w, const char *url);
void http_writer_response(struct http_writer *w, int status);
void http_writer_header(struct http_writer *w, const char *name,
			const char *val);
void http_writer_headerf(struct http_writer *w, const char *name,
			 const char *fmt, ...);
ssize_t http_writer_write(struct http_writer *w, const void *buf, size_t len,
			  struct tasklet *t, struct error *e);

enum http_writer_end_result {
	HTTP_WRITER_END_WAITING,
	HTTP_WRITER_END_ERROR,
	HTTP_WRITER_END_DONE
};

enum http_writer_end_result http_writer_end(struct http_writer *w,
					 struct tasklet *t, struct error *e);

#endif

