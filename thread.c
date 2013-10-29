#include <stdio.h>
#include <string.h>

#include "thread.h"

static void check_pthread_res(const char *where, int res)
{
	if (res == 0)
		return;

	fprintf(stderr, "error: %s: %s\n", where, strerror(res));
	abort();
}

void mutex_init(struct mutex *m)
{
        check_pthread_res("skinny_mutex_init",
                          skinny_mutex_init(&m->mutex));
	m->init = xalloc(1);
	m->held = FALSE;
}

void mutex_fini(struct mutex *m)
{
	assert(!m->held);
	free(m->init);
        check_pthread_res("skinny_mutex_destroy",
                          skinny_mutex_destroy(&m->mutex));
}

void mutex_lock(struct mutex *m)
{
	assert(!m->held);
        check_pthread_res("skinny_mutex_lock",
                          skinny_mutex_lock(&m->mutex));
	m->held = TRUE;
}

void mutex_unlock(struct mutex *m)
{
	assert(m->held);
	m->held = FALSE;
        check_pthread_res("skinny_mutex_unlock",
                          skinny_mutex_unlock(&m->mutex));
}

bool_t mutex_transfer(struct mutex *a, struct mutex *b)
{
	int res;

	assert(a->held);
	a->held = FALSE;
	res = skinny_mutex_transfer(&a->mutex, &b->mutex);
	if (res != EAGAIN) {
		check_pthread_res("skinny_mutex_transfer", res);
		b->held = TRUE;
		return TRUE;
	}
	else {
		a->held = TRUE;
		return FALSE;
	}
}

void mutex_veto_transfer(struct mutex *m)
{
	assert(m->held);
	check_pthread_res("skinny_mutex_veto_transfer",
			  skinny_mutex_veto_transfer(&m->mutex));
}

void cond_init(struct cond *c)
{
	check_pthread_res("pthread_cond_init",
			  pthread_cond_init(&c->cond, NULL));
	c->init = xalloc(1);
}

void cond_fini(struct cond *c)
{
	free(c->init);
	check_pthread_res("pthread_cond_destroy",
			  pthread_cond_destroy(&c->cond));
}

void cond_wait(struct cond *c, struct mutex *m)
{
	mutex_assert_held(m);
	check_pthread_res("skinny_mutex_cond_wait",
			  skinny_mutex_cond_wait(&c->cond, &m->mutex));
}

void cond_signal(struct cond *c)
{
	check_pthread_res("pthread_cond_signal",
			  pthread_cond_signal(&c->cond));
}

void cond_broadcast(struct cond *c)
{
	check_pthread_res("pthread_cond_broadcast",
			  pthread_cond_broadcast(&c->cond));
}
