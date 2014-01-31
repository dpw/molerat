#ifndef MOLERAT_BASE_H
#define MOLERAT_BASE_H

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>

#if __STDC_VERSION__ + 0 < 199900L
#define inline __inline__
#define va_copy(d,s) __va_copy(d,s)
#endif

typedef unsigned char bool_t;
#define TRUE 1
#define FALSE 0

#define PASTE(a,b) PASTE_AUX(a,b)
#define PASTE_AUX(a,b) a##b

#define STATIC_ASSERT(e) typedef char PASTE(_static_assert_,__LINE__)[1-2*!(e)] __attribute__((unused))

#define container_of(ptr, type, member) \
	({ const __typeof__(((type *)0)->member ) *__mptr = (ptr); \
	   ((type *)((char *)__mptr - offsetof(type,member))); })

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define pointer_bits(p) ((uintptr_t)(p) & 3)
#define pointer_clear_bits(p) ((void *)((uintptr_t)(p) & -4))
#define pointer_set_bits(p, bits) ((void *)((uintptr_t)(p) | (bits)))

void die(const char *fmt, ...) __attribute__ ((noreturn,format (printf, 1, 2)));

void *xalloc(size_t s);
void *xrealloc(void *p, size_t s);

char *xsprintf(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

/* Abort on a failed syscall */
void check_syscall(const char *name, int ok);

/* Abort on a failed pthreads call */
void check_pthreads(const char *name, int res);

/* A high-resolution timestamp type.  Signed, so can be used for
   absolute times or time deltas. */
typedef int64_t xtime_t;

#define XTIME_SECOND 1000000

/* Get the current time */
xtime_t time_now(void);

static inline xtime_t xtime_to_ns(xtime_t t)
{
	return t * (1000000000 / XTIME_SECOND);
}

static inline xtime_t xtime_to_ms(xtime_t t)
{
	return t / (XTIME_SECOND / 1000);
}

struct error {
	unsigned int category;
	const char *message;
};

#define ERROR_NONE 0U
#define ERROR_INVALID 1U
#define ERROR_OS 2U
#define ERROR_MISC 3U

static inline void error_init(struct error *e)
{
	e->category = 0;
}

void error_fini(struct error *e);
void error_copy(struct error *src, struct error *dest);
void error_propogate(struct error *src, struct error *dest);

static inline bool_t error_ok(struct error *e)
{
	return !e->category;
}

static inline const char *error_message(struct error *e)
{
	return e->message;
}

void error_reset(struct error *e);
void error_set(struct error *e, unsigned int cat, const char *fmt, ...)
	__attribute__ ((format (printf, 3, 4)));
void error_errno(struct error *e, const char *fmt, ...)
	__attribute__ ((format (printf, 2, 3)));
void error_errno_val(struct error *e, int errnum, const char *fmt, ...)
	__attribute__ ((format (printf, 3, 4)));
void error_invalid(struct error *e, const char *fmt, ...)
	__attribute__ ((format (printf, 2, 3)));

#endif
