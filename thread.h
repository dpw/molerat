#ifndef MOLERAT_THREAD_H
#define MOLERAT_THREAD_H

#include <pthread.h>
#include <assert.h>

#include "skinny-mutex/skinny_mutex.h"
#include "base.h"

struct thread {
	pthread_t id;
	void *init;
};

void thread_init(struct thread *thr, void (*func)(void *data), void *data);
void thread_fini(struct thread *thr);
void thread_signal(struct thread *thr, int sig);

struct mutex {
	skinny_mutex_t mutex;
	bool_t held;
	void *init;
};

struct cond {
	pthread_cond_t cond;
	void *init;
};


void mutex_init(struct mutex *m);
void mutex_fini(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool_t mutex_transfer(struct mutex *a, struct mutex *b);
void mutex_veto_transfer(struct mutex *m);

static inline void mutex_unlock_fini(struct mutex *m)
{
	mutex_unlock(m);
	mutex_fini(m);
}

static inline void mutex_assert_held(struct mutex *m)
{
	assert(m->held);
}


void cond_init(struct cond *c);
void cond_fini(struct cond *c);
void cond_wait(struct cond *c, struct mutex *m);
void cond_signal(struct cond *c);
void cond_broadcast(struct cond *c);

#endif
