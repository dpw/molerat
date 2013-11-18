#include <string.h>

#include <molerat/http_reader.h>
#include <molerat/stream.h>

static int on_url(struct http_parser *hp, const char *at, size_t len);
static int on_url_cont(struct http_parser *hp, const char *at, size_t len);
static int on_header_field(struct http_parser *hp, const char *at, size_t len);
static int on_header_field_cont(struct http_parser *hp, const char *at,
				size_t len);
static int on_header_value(struct http_parser *hp, const char *at, size_t len);
static int on_header_value_cont(struct http_parser *hp, const char *at,
				size_t len);
static int on_headers_complete(struct http_parser *hp);
static int on_message_complete(struct http_parser *hp);
static int on_body(struct http_parser *hp, const char *at, size_t len);

typedef struct http_header_internal {
	/* Fields are offsets into the prebody growbuf */
	int name;
	int name_end;
	int value;
	int value_end;
} header_t;

void http_reader_init(struct http_reader *r, struct stream *stream, bool_t req)
{
	r->state = HTTP_READER_PREBODY;
	r->parsed = 0;
	r->stream = stream;
	growbuf_init(&r->prebody, 1000);
	r->headers_used = 0;
	r->headers_size = 20;
	r->headers = xalloc(r->headers_size * sizeof *r->headers);

	memset(&r->settings, 0, sizeof r->settings);
	r->settings.on_url = on_url;
	r->settings.on_header_field = on_header_field;
	r->settings.on_header_value = on_header_value;
	r->settings.on_headers_complete = on_headers_complete;
	r->settings.on_body = on_body;
	r->settings.on_message_complete = on_message_complete;

	http_parser_init(&r->parser, req ? HTTP_REQUEST : HTTP_RESPONSE);
	r->parser.data = r;
}

void http_reader_fini(struct http_reader *r)
{
	free(r->headers);
	growbuf_fini(&r->prebody);
}

static void set_error_from_parser(struct http_reader *r, struct error *err)
{
	error_set(err, ERROR_MISC, "HTTP error: %s",
		  http_errno_description(HTTP_PARSER_ERRNO(&r->parser)));
}

static int compare_headers(const void *v_a, const void *v_b, void *v_growbuf)
{
	const header_t *a = v_a;
	const header_t *b = v_b;
	struct growbuf *growbuf = v_growbuf;
	int apos, bpos, achar, bchar, res;

	for (apos = a->name, bpos = b->name;
	     ;
	     apos++, bpos++) {
		if (apos == a->name_end)
			break;

		if (bpos == b->name_end)
			return 1;

		achar = *(char *)growbuf_offset(growbuf, apos);
		achar |= ((unsigned)achar - 65U < 26U) << 5;

		bchar = *(char *)growbuf_offset(growbuf, bpos);
		bchar |= ((unsigned)bchar - 65U < 26U) << 5;

		res = achar - bchar;
		if (res != 0)
			return res;
	}

	/* hit the end of a */
	return bpos != b->name_end ? -1 : 0;
}

enum http_reader_prebody_result http_reader_prebody(struct http_reader *r,
						    struct tasklet *tasklet,
						    struct error *err)
{
	enum http_reader_prebody_result waiting_result;

	if (r->state != HTTP_READER_PREBODY) {
		assert(r->state == HTTP_READER_EOM);

		/* Clean up previous request data. */
		growbuf_shift(&r->prebody, r->parsed);
		r->headers_used = 0;
		r->parsed = 0;
		r->state = HTTP_READER_PREBODY;
	}

	waiting_result = HTTP_READER_PREBODY_WAITING;
	for (;;) {
		/* Read some data from the stream */

		/* XXX we should ensure that the prebody buffer
		   doesn't grow beyond INT_MAX, as we store offsets
		   into it as ints. */

		size_t unparsed;
		void *buf = growbuf_reserve(&r->prebody, 100);
		ssize_t rlen = stream_read(r->stream, buf,
					   growbuf_space(&r->prebody),
					   tasklet, err);
		switch (rlen) {
		case STREAM_WAITING:
			return waiting_result;

		case STREAM_ERROR:
			return HTTP_READER_PREBODY_ERROR;

		case STREAM_END:
			/* We need to proceed to tell the parser */
			break;

		default:
			if (rlen == 0)
				continue;

			waiting_result = HTTP_READER_PREBODY_PROGRESS;
			growbuf_advance(&r->prebody, rlen);
			break;
		}

		/* The amount of data in the prebody buffer to be
		   parsed. */
		unparsed = growbuf_length(&r->prebody) - r->parsed;

		do {
			size_t consumed = http_parser_execute(&r->parser,
					 &r->settings,
					 growbuf_offset(&r->prebody, r->parsed),
					 unparsed);
			r->parsed += consumed;
			unparsed -= consumed;
			switch (HTTP_PARSER_ERRNO(&r->parser)) {
			case HPE_OK:
				if (consumed == 0)
					/* Unparsed must have
					   previously been 0, so this
					   must be the first pass
					   through the loop, and the
					   stream read must have
					   returned STREAM_END. */
					return HTTP_READER_PREBODY_CLOSED;

				break;

			case HPE_PAUSED:
				/* Full request has been read */
				assert(r->state == HTTP_READER_BODY);
				http_parser_pause(&r->parser, 0);
				qsort_r(r->headers, r->headers_used,
					sizeof(header_t),
					compare_headers, &r->prebody);
				return HTTP_READER_PREBODY_DONE;

			default:
				/* Error */
				set_error_from_parser(r, err);
				return HTTP_READER_PREBODY_ERROR;
			}
		} while (unparsed);
	}
}

static int on_url(struct http_parser *hp, const char *cat, size_t len)
{
	struct http_reader *r = hp->data;
	char *at = (char *)cat;

	r->url = growbuf_offset_of(&r->prebody, at);
	r->url_end = r->url + len;

	r->settings.on_url = on_url_cont;

	return 0;
}

static int on_url_cont(struct http_parser *hp, const char *cat, size_t len)
{
	struct http_reader *r = hp->data;
	char *at = (char *)cat;
	char *end = growbuf_offset(&r->prebody, r->url_end);

	if (end != at)
		memmove(end, at, len);

	r->url_end += len;
	return 0;
}

static int on_header_field(struct http_parser *hp, const char *cat, size_t len)
{
	struct http_reader *r = hp->data;
	char *at = (char *)cat;
	header_t *header;

	if (++r->headers_used > r->headers_size) {
		r->headers_size *= 2;
		r->headers = xrealloc(r->headers,
				      r->headers_size * sizeof *r->headers);
	}

	header = &r->headers[r->headers_used - 1];
	header->name = growbuf_offset_of(&r->prebody, at);
	header->name_end = header->name + len;

	r->settings.on_header_field = on_header_field_cont;
	r->settings.on_header_value = on_header_value;

	return 0;
}

static int on_header_field_cont(struct http_parser *hp, const char *cat,
				size_t len)
{
	struct http_reader *r = hp->data;
	char *at = (char *)cat;
	header_t *header = &r->headers[r->headers_used - 1];
	char *end = growbuf_offset(&r->prebody, header->name_end);

	if (end != at)
		memmove(end, at, len);

	header->name_end += len;
	return 0;
}

static int on_header_value(struct http_parser *hp, const char *cat, size_t len)
{
	struct http_reader *r = hp->data;
	char *at = (char *)cat;
	header_t *header = &r->headers[r->headers_used - 1];

	header->value = growbuf_offset_of(&r->prebody, at);
	header->value_end = header->value + len;

	r->settings.on_header_field = on_header_field;
	r->settings.on_header_value = on_header_value_cont;

	return 0;
}

static int on_header_value_cont(struct http_parser *hp, const char *cat,
				size_t len)
{
	struct http_reader *r = hp->data;
	char *at = (char *)cat;
	header_t *header = &r->headers[r->headers_used - 1];
	char *end = growbuf_offset(&r->prebody, header->name_end);

	if (end != at)
		memmove(end, at, len);

	header->value_end += len;
	return 0;
}

static int on_headers_complete(struct http_parser *hp)
{
	struct http_reader *r = hp->data;

	r->state = HTTP_READER_BODY;
	r->settings.on_url = on_url;
	r->settings.on_header_value = on_header_value;

	/* pause the http_parser to escape back to http_reader_prebody */
	http_parser_pause(hp, 1);
	return 0;
}

ssize_t http_reader_body(struct http_reader *r, void *v_buf, size_t len,
			 struct tasklet *tasklet, struct error *err)
{
	char *buf = v_buf;
	char *buf_end = buf + len;
	char *buf_pos;
	ssize_t got;

	if (r->state != HTTP_READER_BODY) {
		assert(r->state == HTTP_READER_EOM);
		return STREAM_END;
	}

	r->body_end = buf;

	/* First we need to handle any body data that might be sitting
	   in the prebody buffer. */
	for (;;) {
		size_t prebody_left;

		/* How much space is left in the result buffer?  We
		   can't parse more than this, otherwise we risk
		   overrunning the buffer (http-parser doesn't allow
		   callbacks to accept partial data). */
		len = buf_end - r->body_end;
		if (!len)
			/* Result buffer is full */
			return buf_end - buf;

		/* How much data is left in the prebody buffer */
		 prebody_left = growbuf_length(&r->prebody) - r->parsed;
		if (!prebody_left)
			/* Nothing in the prebody buffer, so read from
			   the stream. */
			break;

		len = http_parser_execute(&r->parser,
				       &r->settings,
				       growbuf_offset(&r->prebody, r->parsed),
				       len < prebody_left ? len : prebody_left);
		r->parsed += len;
		switch (HTTP_PARSER_ERRNO(&r->parser)) {
		case HPE_OK:
			break;

		case HPE_PAUSED:
			/* We have reached the end of the body */
			assert(r->state == HTTP_READER_EOM);
			http_parser_pause(&r->parser, 0);
			goto done;

		default:
			/* Error */
			set_error_from_parser(r, err);
			return -1;
		}
	}

	buf_pos = r->body_end;
	got = stream_read(r->stream, buf_pos, len, tasklet, err);
	switch (got) {
	case STREAM_WAITING:
	case STREAM_ERROR:
		return got;

	case STREAM_END:
		/* We need to tell the parser. */
		got = 0;
		break;

	default:
		if (got == 0)
			return 0;

		break;
	}

	do {
		size_t consumed = http_parser_execute(&r->parser, &r->settings,
						      buf_pos, got);
		buf_pos += consumed;
		got -= consumed;
		switch (HTTP_PARSER_ERRNO(&r->parser)) {
		case HPE_OK:
			break;

		case HPE_PAUSED:
			/* Reached the end of the body. Copy any left
			   over data to the prebody */
			assert(r->state == HTTP_READER_EOM);
			http_parser_pause(&r->parser, 0);
			growbuf_append(&r->prebody, buf_pos, got);
			goto done;

		default:
			/* Error */
			set_error_from_parser(r, err);
			return STREAM_ERROR;
		}
	} while (got);

 done:
	/* All the data read from the stream has been parsed. */
	return r->body_end - buf;
}

static int on_message_complete(struct http_parser *hp)
{
	struct http_reader *r = hp->data;

	r->state = HTTP_READER_EOM;

	/* pause the http_parser to escape back to http_reader_body */
	http_parser_pause(hp, 1);
	return 0;
}

static int on_body(struct http_parser *hp, const char *cat, size_t len)
{
	struct http_reader *r = hp->data;
	char *at = (char *)cat;

	if (r->body_end != at)
		memmove(r->body_end, at, len);

	r->body_end += len;
	return 0;
}

void http_reader_headers(struct http_reader *r, struct http_header_iter *iter)
{
	iter->current = r->headers;
	iter->end = r->headers + r->headers_used;
	iter->base = growbuf_offset(&r->prebody, 0);
}

struct http_header *http_header_iter_next(struct http_header_iter *iter)
{
	if (iter->current == iter->end)
		return NULL;

	iter->header.name = iter->base + iter->current->name;
	iter->header.name_len = iter->current->name_end - iter->current->name;
	iter->header.value = iter->base + iter->current->value;
	iter->header.value_len
		= iter->current->value_end - iter->current->value;
	iter->current++;
	return &iter->header;
}
