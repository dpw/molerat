#include "base.h"
#include "poll.h"
#include "tasklet.h"
#include "application.h"

static struct poll *singleton;

static void singleton_cleanup(void)
{
	struct poll *p = singleton;
	if (p)
		poll_destroy(p);
}

struct poll *poll_singleton(void)
{
	for (;;) {
		struct poll *p = singleton;
		if (p)
			return p;

		p = poll_create();
		if (__sync_bool_compare_and_swap(&singleton, NULL, p)) {
			atexit(singleton_cleanup);
			return p;
		}

		poll_destroy(p);
	}
}

static void poll_thread(void *v_p);

void poll_common_init(struct poll_common *p)
{
	mutex_init(&p->mutex);
	p->thread_woken = TRUE;
	p->thread_stopping = FALSE;
	thread_init(&p->thread, poll_thread, p);
}

void poll_common_wake(struct poll_common *p)
{
	if (!p->thread_woken) {
		p->thread_woken = TRUE;
		thread_signal(thread_get_handle(&p->thread), PRIVATE_SIGNAL);
	}
}

void poll_common_stop(struct poll_common *p)
{
	assert(!p->thread_stopping);
	p->thread_stopping = TRUE;
	poll_common_wake(p);
	mutex_unlock(&p->mutex);
	thread_fini(&p->thread);
	mutex_lock(&p->mutex);
}

static void poll_thread(void *v_p)
{
	struct poll_common *p = v_p;
	struct run_queue *runq = run_queue_create();
	sigset_t sigmask;

	application_assert_prepared();
	run_queue_target(runq);

	check_pthreads("pthread_sigmask",
		       pthread_sigmask(SIG_SETMASK, NULL, &sigmask));
	sigdelset(&sigmask, PRIVATE_SIGNAL);

	for (;;) {
		mutex_lock(&p->mutex);
		poll_prepare((struct poll *)p);

		if (p->thread_stopping)
			break;

		p->thread_woken = FALSE;
		mutex_unlock(&p->mutex);

		if (!poll_poll((struct poll *)p, &sigmask))
			continue;

		mutex_lock(&p->mutex);
		p->thread_woken = TRUE;
		poll_dispatch((struct poll *)p);
		mutex_unlock(&p->mutex);

		run_queue_run(runq, FALSE);
	}

	mutex_unlock(&p->mutex);
}
