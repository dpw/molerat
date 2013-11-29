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
	struct bytes buf = c_string_bytes("foobar");
	assert(bytes_check(&buf, "foo"));
	assert(bytes_check(&buf, "bar"));
}

static bool_t growbuf_check(struct growbuf *gbuf, char *s)
{
	return !bytes_compare(growbuf_to_bytes(gbuf), c_string_bytes(s));
}

static void test_growbuf(void)
{
	struct growbuf buf;

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
	growbuf_to_bytes(&buf);
	assert(growbuf_frozen(&buf));
	growbuf_fini(&buf);
}

int main(void)
{
	test_bytes();
	test_growbuf();
	return 0;
}
