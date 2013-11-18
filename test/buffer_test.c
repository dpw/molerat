#include <string.h>
#include <assert.h>

#include <molerat/buffer.h>

static bool_t drainbuf_check(struct drainbuf *buf, char *s)
{
	size_t l = strlen(s);

	if (memcmp(s, drainbuf_current(buf), l) != 0)
		return FALSE;

	drainbuf_advance(buf, l);
	return TRUE;
}

static void test_drainbuf(void)
{
	struct drainbuf buf;
	char s[] = "foobar";

	drainbuf_set(&buf, s, strlen(s));
	assert(drainbuf_check(&buf, "foo"));
	assert(drainbuf_check(&buf, "bar"));
}

static bool_t growbuf_check(struct growbuf *gbuf, char *s)
{
	size_t l = strlen(s);
	struct drainbuf dbuf;

	growbuf_to_drainbuf(gbuf, &dbuf);
	return l == drainbuf_length(&dbuf)
		&& memcmp(s, drainbuf_current(&dbuf), l) == 0;
}

static void test_growbuf(void)
{
	struct growbuf buf;
	struct drainbuf dbuf;

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
	growbuf_to_drainbuf(&buf, &dbuf);
	assert(growbuf_frozen(&buf));
	growbuf_fini(&buf);
}

int main(void)
{
	test_drainbuf();
	test_growbuf();
	return 0;
}
