#ifndef MOLERAT_POLL_H
#define MOLERAT_POLL_H

#include <signal.h>

#include "thread.h"
#include "watched_fd.h"

/* Stuff common to all poll implementations */

struct poll_common {
	struct mutex mutex;

	struct thread thread;
	bool_t thread_woken;
	bool_t thread_stopping;
};

struct poll *poll_singleton(void);
void poll_common_init(struct poll_common *p);
void poll_common_wake(struct poll_common *p);
void poll_common_stop(struct poll_common *p);

/* The remaining functions are provided by the specific poll implementation. */
struct poll *poll_create(void);
void poll_destroy(struct poll *p);

void poll_prepare(struct poll *p);
bool_t poll_poll(struct poll *p, sigset_t *sigmask);
void poll_dispatch(struct poll *p);

#endif
