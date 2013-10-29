#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

/* Arghhhh.  This is to get the POSIX-compliant strerror_r. */
#define USE_GNU_DEFINED defined(__USE_GNU)
#undef __USE_GNU
#include <string.h>
#if USE_GNU_DEFINED
#define __USE_GNU
#endif
#undef USE_GNU_DEFINED

#include "base.h"


void *xalloc(size_t s)
{
	void *res = malloc(s);
	if (res)
		return res;

	fprintf(stderr, "malloc(%ld) failed\n", (long)s);
	abort();
	return NULL;
}

void *xrealloc(void *p, size_t s)
{
	if (s) {
		void *res = realloc(p, s);
		if (res)
			return res;

		fprintf(stderr, "malloc(%ld) failed\n", (long)s);
		abort();
	}
	else {
		free(p);
	}

	return NULL;
}

size_t xsprintf(char **buf, const char *fmt, ...)
{
	int res;
	va_list ap;
	va_start(ap, fmt);

	res = vasprintf(buf, fmt, ap);
	va_end(ap);
	if (res >= 0)
		return res;

	fprintf(stderr, "asprintf failed\n");
	abort();
}

static const char fallback_message[] = "<unable to format message>";

void error_fini(struct error *e)
{
	if (e->category && e->message != fallback_message)
		free((void *)e->message);
}

void error_copy(struct error *src, struct error *dest)
{
	error_fini(dest);
	dest->category = src->category;
	if (src->category)
		dest->message = strdup(src->message);
}

void error_propogate(struct error *src, struct error *dest)
{
	error_fini(dest);
	dest->category = src->category;
	dest->message = src->message;
}

void error_reset(struct error *e)
{
	error_fini(e);
	e->category = 0;
}

void error_set_va(struct error *e, unsigned int cat, const char *fmt,
		  va_list ap)
{
	char *message;
	int res = vasprintf(&message, fmt, ap);

	error_fini(e);
	e->category = cat;
	e->message = (res >= 0) ? message :  fallback_message;
}

void error_set(struct error *e, unsigned int cat, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	error_set_va(e, cat, fmt, ap);
	va_end(ap);
}

static char *errno_message(int en)
{
	size_t sz = 100;
	char *buf;

	for (;;) {
		bool_t failed;

		buf = malloc(sz);

		// The "* 1" here is to make sure we have the POSIX
		// strerror_r, not the GNU variant which returns a pointer.
		if (!buf || strerror_r(en, buf, sz) * 1 >= 0)
			break;

		failed = (errno != ERANGE);
		free(buf);
		buf = NULL;

		if (failed)
			break;

		sz *= 2;
	}

	return buf;
}

static void error_errno_val_va(struct error *e, int errnum, const char *fmt,
			       va_list ap)
{
	char *en_msg = errno_message(errnum);
	char *msg;

	if (vasprintf(&msg, fmt, ap) < 0)
		msg = NULL;

	error_set(e, ERROR_OS, "%s: %s",
		  msg ? msg : fallback_message,
		  en_msg ? en_msg : fallback_message);

	free(msg);
	free(en_msg);
}

void error_errno(struct error *e, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	error_errno_val_va(e, errno, fmt, ap);
	va_end(ap);
}

void error_errno_val(struct error *e, int errnum, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	error_errno_val_va(e, errnum, fmt, ap);
	va_end(ap);
}

void error_invalid(struct error *e, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	error_set_va(e, ERROR_INVALID, fmt, ap);
	va_end(ap);
}
