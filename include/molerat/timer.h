#ifndef MOLERAT_TIMER_H
#define MOLERAT_TIMER_H

#include <molerat/tasklet.h>

struct timer {
	struct poll_common *poll;
	xtime_t earliest;
	xtime_t latest;
	bool_t fired;
	struct timer *next;
	struct timer *prev;
	struct wait_list waiting;
};

void timer_init(struct timer *t);
void timer_fini(struct timer *t);
void timer_set(struct timer *t, xtime_t earliest, xtime_t latest);
void timer_set_relative(struct timer *t, xtime_t earliest, xtime_t latest);
void timer_cancel(struct timer *t);
bool_t timer_wait(struct timer *t, struct tasklet *tasklet);

#endif
