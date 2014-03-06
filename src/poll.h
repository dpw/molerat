#ifndef MOLERAT_POLL_H
#define MOLERAT_POLL_H

#include <molerat/thread.h>
#include <molerat/watched_fd.h>
#include <molerat/timer.h>

/* Stuff common to all poll implementations */

struct poll_common {
	struct mutex mutex;

	struct thread thread;
	bool_t thread_woken;
	bool_t thread_stopping;

	struct timer *timers;
};

struct poll *poll_singleton(void);
void poll_common_init(struct poll_common *p);
void poll_common_wake(struct poll_common *p);
void poll_common_stop(struct poll_common *p);

/* The remaining functions are provided by the specific poll implementation. */
struct poll *poll_create(void);
void poll_destroy(struct poll *p);

void poll_thread_init(struct poll *p);
void poll_prepare(struct poll *p);
void poll_poll(struct poll *p, xtime_t timeout);
void poll_dispatch(struct poll *p);
void poll_wake(struct poll *p);

#endif
