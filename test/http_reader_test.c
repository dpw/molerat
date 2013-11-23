#include <string.h>

#include <molerat/buffer.h>
#include <molerat/http_reader.h>

#include "stream_utils.h"

static int compare_growbufs(const void *a, const void *b)
{
	int res;
	size_t len = growbuf_length(a);
	if (growbuf_length(b) < len)
		len = growbuf_length(b);

	res = memcmp(growbuf_offset(a, 0), growbuf_offset(b, 0), len);
	if (res)
		return res;

	return (growbuf_length(a) > len) - (growbuf_length(b) > len);
}

static void check_headers(struct http_reader *r, int count, const char *expect)
{
	struct http_header_iter iter;
	struct http_header *header;
	struct growbuf *bufs = xalloc(count * sizeof *bufs);
	int i;

	for (i = 0, http_reader_headers(r, &iter);
	     (header = http_header_iter_next(&iter));
	     i++) {
		assert(i < count);
		growbuf_init(&bufs[i], 100);
		growbuf_printf(&bufs[i], "<%.*s>=<%.*s>",
			       (int)header->name_len, header->name,
			       (int)header->value_len, header->value);
	}

	assert(i == count);
	qsort(bufs, count, sizeof(struct growbuf), compare_growbufs);

	for (i = 1; i < count; i++) {
		growbuf_append_string(&bufs[0], ",");
		growbuf_append_growbuf(&bufs[0], &bufs[i]);
	}

	assert(growbuf_length(&bufs[0]) == strlen(expect));
	assert(memcmp(growbuf_offset(&bufs[0], 0), expect,
		      growbuf_length(&bufs[0])) == 0);

	for (i = 0; i < count; i++)
		growbuf_fini(&bufs[i]);

	free(bufs);
}

const char test_data[] =
	"GET / HTTP/1.1\r\n"
	"Host: foo.example.com\r\n"
	"User-Agent: UA1\r\n"
	"\r\n"
	"GET / HTTP/1.1\r\n"
	"Host: bar.example.com\r\n"
	"User-Agent: UA2\r\n"
	"\r\n";

int main(void)
{
	struct drainbuf buf;
	struct stream *stream;
	struct http_reader reader;
	struct error err;
	char c[1];

	drainbuf_set(&buf, test_data, strlen(test_data));
	stream = drainbuf_read_stream_create(&buf);

	http_reader_init(&reader, stream, TRUE);

	assert(http_reader_prebody(&reader, NULL, &err)
	                                        == HTTP_READER_PREBODY_DONE);
	check_headers(&reader, 2, "<Host>=<foo.example.com>,<User-Agent>=<UA1>");
	assert(http_reader_body(&reader, c, 0, NULL, &err) == 0);
	assert(http_reader_body(&reader, c, 1, NULL, &err) == STREAM_END);

	assert(http_reader_prebody(&reader, NULL, &err)
	                                        == HTTP_READER_PREBODY_DONE);
	check_headers(&reader, 2, "<Host>=<bar.example.com>,<User-Agent>=<UA2>");
	assert(http_reader_body(&reader, c, 1, NULL, &err) == STREAM_END);

	assert(http_reader_prebody(&reader, NULL, &err)
	                                        == HTTP_READER_PREBODY_CLOSED);

	http_reader_fini(&reader);
	stream_destroy(stream);

	return 0;
}
