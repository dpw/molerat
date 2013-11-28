#include <string.h>
#include <assert.h>

#include <molerat/buffer.h>

static bool_t bytes_check(struct bytes *buf, char *s)
{
	size_t l = strlen(s);

	if (memcmp(s, bytes_current(*buf), l) != 0)
		return FALSE;

	bytes_advance(buf, l);
	return TRUE;
}

static void test_bytes(void)
{
	char s[] = "foobar";
	struct bytes buf = make_bytes(s, strlen(s));
	assert(bytes_check(&buf, "foo"));
	assert(bytes_check(&buf, "bar"));
}

static bool_t growbuf_check(struct growbuf *gbuf, char *s)
{
	size_t l = strlen(s);
	struct bytes dbuf;

	growbuf_to_bytes(gbuf, &dbuf);
	return l == bytes_length(dbuf)
		&& memcmp(s, bytes_current(dbuf), l) == 0;
}

static void test_growbuf(void)
{
	struct growbuf buf;
	struct bytes dbuf;

	growbuf_init(&buf, 2);
	growbuf_append_string(&buf, "hello, ");
	growbuf_append_string(&buf, "world");
	growbuf_append_string(&buf, "!");
	assert(growbuf_check(&buf, "hello, world!"));
	growbuf_fini(&buf);

	growbuf_init(&buf, 2);
	growbuf_printf(&buf, "hello, %s!", "world");
	assert(growbuf_check(&buf, "hello, world!"));
	growbuf_fini(&buf);

	growbuf_init(&buf, 2);
	assert(!growbuf_frozen(&buf));
	growbuf_to_bytes(&buf, &dbuf);
	assert(growbuf_frozen(&buf));
	growbuf_fini(&buf);
}

int main(void)
{
	test_bytes();
	test_growbuf();
	return 0;
}
