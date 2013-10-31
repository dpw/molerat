#ifndef MOLERAT_BASE_H
#define MOLERAT_BASE_H

#include <stdlib.h>
#include <stdint.h>

typedef unsigned char bool_t;
#define TRUE 1
#define FALSE 0

#define pointer_bits(p) ((uintptr_t)(p) & 3)
#define pointer_clear_bits(p) ((void *)((uintptr_t)(p) & -4))
#define pointer_set_bits(p, bits) ((void *)((uintptr_t)(p) | (bits)))

void *xalloc(size_t s);
void *xrealloc(void *p, size_t s);
size_t xsprintf(char **buf, const char *fmt, ...);

/* Abort on a failed syscall */
void check_syscall(const char *name, int ok);

/* Abort on a failed pthreads call */
void check_pthreads(const char *name, int res);

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
void error_set(struct error *e, unsigned int cat, const char *fmt, ...);
void error_errno(struct error *e, const char *fmt, ...);
void error_errno_val(struct error *e, int errnum, const char *fmt, ...);
void error_invalid(struct error *e, const char *fmt, ...);

#endif
