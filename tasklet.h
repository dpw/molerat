#ifndef MOLERAT_TASKLET_H
#define MOLERAT_TASKLET_H

#include "base.h"
#include "thread.h"

struct tasklet {
	struct mutex *mutex;

	void (*handler)(void *);
	void *data;

	struct mutex wait_mutex;
	struct wait_list *wait;	   /* Covered by wait_mutex */
	int unwaiting;		   /* Covered by wait_mutex */
	bool_t waited;
	struct tasklet *wait_next; /* Covered by wait's mutex */
	struct tasklet *wait_prev; /* Ditto */

	struct run_queue *runq; /* Set using atomic ops */
	struct tasklet *runq_next; /* Covered by runq's mutex */
	struct tasklet *runq_prev; /* Ditto */
};

struct wait_list {
	struct mutex mutex;
	void *head;
	int unwaiting;
	int up_count;
};

void tasklet_init(struct tasklet *tasklet, struct mutex *mutex,
		  void *data);
void tasklet_fini(struct tasklet *t);
void tasklet_stop(struct tasklet *t);
void tasklet_run(struct tasklet *t);

static inline void tasklet_now(struct tasklet *t, void (*handler)(void *))
{
	mutex_assert_held(t->mutex);
	t->handler = handler;
	handler(t->data);
}

static inline void tasklet_later(struct tasklet *t, void (*handler)(void *))
{
	mutex_assert_held(t->mutex);
	t->handler = handler;
	tasklet_run(t);
}

void wait_list_init(struct wait_list *w, int up_count);
void wait_list_fini(struct wait_list *w);
void wait_list_up(struct wait_list *w, int n);
bool_t wait_list_down(struct wait_list *w, int n, struct tasklet *t);
void wait_list_set(struct wait_list *w, int n, bool_t broadcast);
bool_t wait_list_nonempty(struct wait_list *w);

void wait_list_wait(struct wait_list *w, struct tasklet *t);
void wait_list_broadcast(struct wait_list *w);

void run_queue_thread_run(void);

#endif
