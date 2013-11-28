#ifndef MOLERAT_HTTP_READER_H
#define MOLERAT_HTTP_READER_H

#include <molerat/buffer.h>
#include <http-parser/http_parser.h>

struct socket;
struct tasklet;

struct http_reader {
	enum { HTTP_READER_PREBODY, HTTP_READER_BODY, HTTP_READER_EOM } state;

	/* The offset in prebody reached by the http_parser */
	int parsed;

	int url;
	int url_end;

	/* Used when parsing the message body to record the end of the
	   contiguous body data. */
	char *body_end;

	struct stream *stream;
	struct growbuf prebody;
	struct http_header_internal *headers;
	int headers_used;
	int headers_size;
	struct http_parser_settings settings;
	struct http_parser parser;
};

struct http_header {
	const char *name;
	size_t name_len;
	const char *value;
	size_t value_len;
};

struct http_header_iter {
	struct http_header header;
	struct http_header_internal *current;
	struct http_header_internal *end;
	char *base;
};


void http_reader_init(struct http_reader *r, struct stream *stream, bool_t req);
void http_reader_fini(struct http_reader *r);

/* Result codes from http_reader_prebody */
enum http_reader_prebody_result {
	/* The HTTP connection was closed while waiting for a message
	   to start. */
	HTTP_READER_PREBODY_CLOSED,

	/* The prebody is done, start reading the body. */
	HTTP_READER_PREBODY_DONE,

	/* The tasklet has been placed on a wait list until progress
	   can be made. */
	HTTP_READER_PREBODY_WAITING,

	/* Data was received and the tasklet made some progress, but
	   still ended up waiting. */
	HTTP_READER_PREBODY_PROGRESS,

	/* An error occurred. */
	HTTP_READER_PREBODY_ERROR
};

/* Continue to read the prebody (i.e. the request/response line plus
   headers. */
enum http_reader_prebody_result http_reader_prebody(struct http_reader *r,
						    struct tasklet *tasklet,
						    struct error *err);

/* Read the message body.  The return value has the same conventions
   as stream_read. */
ssize_t http_reader_body(struct http_reader *r, void *v_buf, size_t len,
			 struct tasklet *tasklet, struct error *err);

static inline enum http_method http_reader_method(struct http_reader *r)
{
	return r->parser.method;
}

/* Get the request URL.  Remains valid from when http_reader_prebody
   indicates it is finished until the next call to
   http_reaader_prebody. */
struct bytes http_reader_url(struct http_reader *r);

/* Get an iterator over the headers.  Lifetime of the header are as
   for http_reader_url. */
void http_reader_headers(struct http_reader *r, struct http_header_iter *iter);
struct http_header *http_header_iter_next(struct http_header_iter *iter);

#endif
