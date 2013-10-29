#ifndef MOLERAT_HTTP_READER_H
#define MOLERAT_HTTP_READER_H

#include "buffer.h"
#include "http-parser/http_parser.h"

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

	struct socket *socket;
	struct growbuf prebody;
	struct http_header *headers;
	int headers_used;
	int headers_size;
	struct http_parser_settings settings;
	struct http_parser parser;
};

void http_reader_init(struct http_reader *r, struct socket *socket, bool_t req);
void http_reader_fini(struct http_reader *r);

enum http_reader_prebody_result {
	HTTP_READER_PREBODY_CLOSED,
	HTTP_READER_PREBODY_DONE,
	HTTP_READER_PREBODY_BLOCKED,
	HTTP_READER_PREBODY_ERROR
};

enum http_reader_prebody_result http_reader_prebody(struct http_reader *r,
						    struct tasklet *tasklet,
						    struct error *err);
ssize_t http_reader_body(struct http_reader *r, void *v_buf, size_t len,
			 struct tasklet *tasklet, struct error *err);

#endif
