#include <molerat/base.h>
#include <molerat/tasklet.h>
#include <molerat/application.h>

#include "poll.h"

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
	p->timers = NULL;
	thread_init(&p->thread, poll_thread, p);
}

void poll_common_wake(struct poll_common *p)
{
	if (!p->thread_woken) {
		p->thread_woken = TRUE;
		poll_wake((struct poll *)p);
	}
}

void poll_common_stop(struct poll_common *p)
{
	assert(!p->thread_stopping);
	p->thread_stopping = TRUE;
	poll_common_wake(p);
	mutex_unlock(&p->mutex);
	thread_fini(&p->thread);
	assert(!p->timers);
	mutex_lock(&p->mutex);
}

void timer_init(struct timer *t)
{
	t->poll = (struct poll_common *)poll_singleton();
	t->next = NULL;
	t->fired = FALSE;
	wait_list_init(&t->waiting, 0);
}

static void timer_cancel_locked(struct timer *t)
{
	struct poll_common *p = t->poll;

	if (t->next) {
		if (t->next != t) {
			t->next->prev = t->prev;
			t->prev->next = t->next;
			if (p->timers == t)
				p->timers = t->next;
		}
		else {
			p->timers = NULL;
		}
	}

	poll_common_wake(p);
}

void timer_fini(struct timer *t)
{
	struct poll_common *p = t->poll;
	mutex_lock(&p->mutex);
	timer_cancel_locked(t);
	t->poll = NULL;
	wait_list_fini(&t->waiting);
	mutex_unlock(&p->mutex);
}

void timer_set(struct timer *t, xtime_t earliest, xtime_t latest)
{
	struct poll_common *p = t->poll;
	assert(earliest < latest);
	mutex_lock(&p->mutex);
	t->earliest = earliest;
	t->latest = latest;
	t->fired = FALSE;

	if (!t->next) {
		struct timer *head = p->timers;
		if (head != NULL) {
			struct timer *tail = head->prev;
			t->next = head;
			t->prev = tail;
			head->prev = tail->next = t;
		}
		else {
			p->timers = t;
			t->prev = t->next = t;
		}
	}

	poll_common_wake(p);
	mutex_unlock(&p->mutex);
}

void timer_set_relative(struct timer *t, xtime_t earliest, xtime_t latest)
{
	xtime_t now = time_now();
	timer_set(t, earliest + now, latest + now);
}

void timer_cancel(struct timer *t)
{
	struct poll_common *p = t->poll;
	mutex_lock(&p->mutex);
	timer_cancel_locked(t);
	t->next = NULL;
	t->fired = FALSE;
	mutex_unlock(&p->mutex);
}

bool_t timer_wait(struct timer *t, struct tasklet *tasklet)
{
	bool_t res = TRUE;
	mutex_lock(&t->poll->mutex);

	if (!t->fired) {
		wait_list_wait(&t->waiting, tasklet);
		res = FALSE;
	}

	mutex_unlock(&t->poll->mutex);
	return res;
}

static void dispatch_timers(struct poll_common *p)
{
	xtime_t now;
	struct timer *t = p->timers;

	if (!t)
		return;

	now = time_now();

	for (;;) {
		struct timer *next = t->next;

		if (t->earliest <= now) {
			t->fired = TRUE;
			t->next = NULL;
			wait_list_broadcast(&t->waiting);

			if (next != t) {
				next->prev = t->prev;
				t->prev->next = next;
				if (p->timers == t) {
					t = p->timers = next;
					continue;
				}
			}
			else {
				p->timers = NULL;
				break;
			}
		}

		t = next;
		if (t == p->timers)
			break;
	}
}

static xtime_t earliest_latest(struct poll_common *p)
{
	struct timer *t = p->timers;
	xtime_t latest;

	if (t == NULL)
		return -1;

	latest = t->latest;
	for (;;) {
		t = t->next;
		if (t == p->timers)
			break;

		if (t->latest < latest)
			latest = t->latest;
	}

	return latest;
}

static void poll_thread(void *v_p)
{
	struct poll_common *p = v_p;
	struct run_queue *runq = run_queue_create();
	xtime_t timeout;

	application_assert_prepared();
	run_queue_target(runq);

	poll_thread_init((struct poll *)p);

	for (;;) {
		mutex_lock(&p->mutex);
		if (p->thread_stopping)
			break;

		poll_prepare((struct poll *)p);
		timeout = earliest_latest(p);
		p->thread_woken = FALSE;
		mutex_unlock(&p->mutex);

		if (timeout < 0 || (timeout -= time_now()) > 0)
			poll_poll((struct poll *)p, timeout);

		mutex_lock(&p->mutex);
		if (p->thread_stopping)
			break;

		p->thread_woken = TRUE;
		poll_dispatch((struct poll *)p);
		dispatch_timers(p);
		mutex_unlock(&p->mutex);

		run_queue_run(runq, FALSE);
	}

	/* poll_prepare may perform some housekeeping that should
	   occur before we finish. */
	poll_prepare((struct poll *)p);
	mutex_unlock(&p->mutex);
}

