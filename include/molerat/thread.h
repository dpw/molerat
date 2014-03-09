#ifndef MOLERAT_THREAD_H
#define MOLERAT_THREAD_H

#include <pthread.h>
#include <assert.h>

#include <skinny-mutex/skinny_mutex.h>
#include <molerat/base.h>

typedef pthread_t thread_handle_t;
static inline thread_handle_t thread_handle_current(void)
{
	return pthread_self();
}

struct thread {
	thread_handle_t handle;
	void *init;
};

void thread_init(struct thread *thr, void (*func)(void *data), void *data);
void thread_fini(struct thread *thr);

static inline thread_handle_t thread_get_handle(struct thread *thr)
{
	return thr->handle;
}

void thread_signal(thread_handle_t thr, int sig);

struct mutex {
	skinny_mutex_t mutex;
	bool_t held;
	void *init;
};

struct cond {
	pthread_cond_t cond;
	void *init;
};


#define MUTEX_INITIALIZER { SKINNY_MUTEX_INITIALIZER, FALSE, NULL }

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


#ifndef USE_PTHREAD_TLS
#define TLS_VAR_DECLARE_STATIC(name) static __thread void *name;
#define TLS_VAR_GET(name) name
#define TLS_VAR_SET(name, val) name = (val)
#else
struct tls_var {
	pthread_once_t once;
	pthread_key_t key;
};

#define TLS_VAR_DECLARE_STATIC(name)                                  \
static pthread_once_t name##_once = PTHREAD_ONCE_INIT;                \
static pthread_key_t name##_key;                                      \
                                                                      \
static void name##_once_func(void) {                                  \
	check_pthreads("pthread_key_create",                          \
		       pthread_key_create(&name##_key, NULL));        \
}

#define TLS_VAR_GET(name)                                             \
(check_pthreads("pthread_once", pthread_once(&name##_once, name##_once_func)), \
 pthread_getspecific(name##_key))

#define TLS_VAR_SET(name, val)                                        \
(check_pthreads("pthread_once", pthread_once(&name##_once, name##_once_func)), \
 check_pthreads("pthread_setspecific", pthread_setspecific(name##_key, val)))
#endif

#endif
