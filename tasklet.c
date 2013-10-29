#include "tasklet.h"

void tasklet_init(struct tasklet *tasklet, struct mutex *mutex, void *data)
{
	tasklet->mutex = mutex;
	tasklet->handler = NULL;
	tasklet->data = data;
	mutex_init(&tasklet->wait_mutex);
	tasklet->wait = NULL;
	tasklet->unwaiting = 0;
	tasklet->runq = NULL;
}

struct run_queue {
	struct mutex mutex;
	struct tasklet *head;
	struct tasklet *current;
	bool_t current_stopped;
	bool_t current_requeue;

	bool_t waiting;
	pthread_t thread;
	struct cond cond;
};

struct run_queue *run_queue_create(void)
{
	struct run_queue *runq = xalloc(sizeof *runq);

	mutex_init(&runq->mutex);
	runq->head = runq->current = NULL;
	runq->waiting = FALSE;
	cond_init(&runq->cond);

	return runq;
}

void run_queue_destroy(struct run_queue *runq)
{
	assert(!runq->head);
	assert(!runq->current);
	mutex_fini(&runq->mutex);
	cond_fini(&runq->cond);
	free(runq);
}


static struct run_queue *default_run_queue;

static void thread_run_queue_cleanup(void)
{
	struct run_queue *runq = default_run_queue;
	if (runq)
		run_queue_destroy(runq);
}

/* Not really a per-thread run-queue at the moment */
static struct run_queue *thread_run_queue(void)
{
	for (;;) {
		struct run_queue *runq = default_run_queue;
		if (runq)
			return runq;

		runq = run_queue_create();
		if (__sync_bool_compare_and_swap(&default_run_queue, NULL,
						 runq)) {
			atexit(thread_run_queue_cleanup);
			return runq;
		}

		run_queue_destroy(runq);
	}
}

static void run_queue_enqueue(struct run_queue *runq, struct tasklet *t)
{
	struct tasklet *head;

	mutex_assert_held(&runq->mutex);
	assert(t->runq == runq);

	head = runq->head;
	if (!head) {
		runq->head = t->runq_next = t->runq_prev = t;
	}
	else {
		struct tasklet *prev = head->runq_prev;
		t->runq_next = head;
		t->runq_prev = prev;
		prev->runq_next = head->runq_prev = t;
	}
}

static void run_queue_remove(struct run_queue *runq, struct tasklet *t)
{
	struct tasklet *next, *prev;

	mutex_assert_held(&runq->mutex);
	assert(t->runq == runq);

	next = t->runq_next;
	prev = t->runq_prev;
	next->runq_prev = prev;
	prev->runq_next = next;

	if (runq->head == t)
		runq->head = (next == t ? NULL : next);
}

/* The tasklet lock does not need to be held for this. */
void tasklet_run(struct tasklet *t)
{
	bool_t done = FALSE;

	do {
		struct run_queue *runq = t->runq;
		if (!runq) {
			runq = thread_run_queue();
			mutex_lock(&runq->mutex);

			if (__sync_bool_compare_and_swap(&t->runq, NULL,
							 runq)) {
				run_queue_enqueue(runq, t);
				done = TRUE;
			}
		}
		else {
			mutex_lock(&runq->mutex);

			if (t->runq == runq) {
				if (runq->current == t)
					runq->current_requeue = TRUE;

				done = TRUE;
			}
		}

		mutex_unlock(&runq->mutex);
	} while (!done);
}


void wait_list_init(struct wait_list *w, int up_count)
{
	mutex_init(&w->mutex);
	w->head = NULL;
	w->unwaiting = 0;
	w->up_count = up_count;
}

void wait_list_fini(struct wait_list *w)
{
	struct tasklet *head;

	mutex_lock(&w->mutex);

	head = w->head;
	if (head) {
		struct tasklet *t = head;

		/* Remove all tasklets from the linked list */
		do {
			struct tasklet *next = t->wait_next;

			tasklet_run(t);

			mutex_lock(&t->wait_mutex);
			w->unwaiting += t->unwaiting;
			t->wait = NULL;
			t->unwaiting = 0;
			mutex_unlock(&t->wait_mutex);

			t = next;
		} while (t != head);

		/* If other threads are waiting on the wait_list mutex
		   to remove themselves, allow them to proceed and
		   wait until they are done. */
		if (w->unwaiting) {
			struct cond cond;

			cond_init(&cond);
			w->head = pointer_set_bits(&cond, 1);

			do {
				cond_wait(&cond, &w->mutex);
			} while (w->unwaiting);

			cond_fini(&cond);
		}

		w->head = NULL;
	}

	mutex_unlock_fini(&w->mutex);
}

static void tasklet_unwait(struct tasklet *t)
{
	struct wait_list *w;
	struct tasklet *next;

	mutex_lock(&t->wait_mutex);

	for (;;) {
		w = t->wait;
		if (!w) {
			/* Tasklet is not on a wait_list */
			mutex_unlock(&t->wait_mutex);
			return;
		}

		t->unwaiting++;
		mutex_unlock(&t->wait_mutex);
		mutex_lock(&w->mutex);
		mutex_lock(&t->wait_mutex);

		/* The tasklet could have been removed from the
		   waitlist, or even be on a different waitlist by
		   now. If so, we need to start again. */
		if (t->wait == w)
			break;

		if (!--w->unwaiting && pointer_bits(w->head)) {
			/* Dropping the last reference to the
			   wait_list, so wake up the wait_list_fini
			   caller. */
 			struct cond *cond = pointer_clear_bits(w->head);
			cond_signal(cond);
		}

		mutex_unlock(&w->mutex);
	}

	/* Remove t from the waitlist */
	t->wait = NULL;

	/* Other threads may be accounted for in t->unwaiting.
	   We need to record them in the wait_list. */
	w->unwaiting += t->unwaiting - 1;
	t->unwaiting = 0;

	next = t->wait_next;
	t->wait_prev->wait_next = next;
	next->wait_prev = t->wait_prev;

	if (w->head == t) {
		if (next == t) {
			w->head = NULL;
		}
		else {
			w->head = next;
			if (w->up_count)
				tasklet_run(next);
		}
	}

	mutex_unlock(&t->wait_mutex);
	mutex_unlock(&w->mutex);
}

static void wait_list_broadcast_locked(struct wait_list *w)
{
	struct tasklet *head;

	mutex_assert_held(&w->mutex);

	head = w->head;
	if (head) {
		struct tasklet *t = head;
		do {
			tasklet_run(t);
			t = t->wait_next;
		} while (t != head);
	}
}

void wait_list_broadcast(struct wait_list *w)
{
	mutex_lock(&w->mutex);
	wait_list_broadcast_locked(w);
	mutex_unlock(&w->mutex);
}

void wait_list_set(struct wait_list *w, int n, bool_t broadcast)
{
	mutex_lock(&w->mutex);
	w->up_count = n;
	if (broadcast)
		wait_list_broadcast_locked(w);
	mutex_unlock(&w->mutex);
}

static bool_t wait_list_add(struct wait_list *w, struct tasklet *t)
{
	bool_t res = FALSE;

	mutex_lock(&t->wait_mutex);

	if (t->wait)
		goto out;

	t->wait = w;

	if (!w->head) {
		w->head = t->wait_next = t->wait_prev = t;
		if (w->up_count)
			tasklet_run(t);
	}
	else {
		struct tasklet *head = w->head;
		struct tasklet *prev = head->wait_prev;
		t->wait_next = head;
		t->wait_prev = prev;
		head->wait_prev = prev->wait_next = t;
	}

	res = TRUE;
 out:
	mutex_unlock(&t->wait_mutex);
	return res;
}

void wait_list_wait(struct wait_list *w, struct tasklet *t)
{
	mutex_lock(&w->mutex);

	for (;;) {
		if (t->wait == w || wait_list_add(w, t))
			break;

		mutex_unlock(&w->mutex);
		tasklet_unwait(t);
		mutex_lock(&w->mutex);
	}

	mutex_unlock(&w->mutex);
	t->waited = TRUE;
}

bool_t wait_list_down(struct wait_list *w, int n, struct tasklet *t)
{
	bool_t res = FALSE;

	mutex_lock(&w->mutex);

	for (;;) {
		if (w->up_count >= n) {
			w->up_count -= n;
			res = TRUE;
			break;
		}

		if (t->wait == w || wait_list_add(w, t))
			break;

		mutex_unlock(&w->mutex);
		tasklet_unwait(t);
		mutex_lock(&w->mutex);
	}

	mutex_unlock(&w->mutex);
	t->waited = TRUE;
	return res;
}

void wait_list_up(struct wait_list *w, int n)
{
	mutex_lock(&w->mutex);

	w->up_count += n;
	if (w->head)
		tasklet_run(w->head);

	mutex_unlock(&w->mutex);
}

bool_t wait_list_nonempty(struct wait_list *w)
{
	return !!w->head;
}

void run_queue_run(struct run_queue *runq)
{
	mutex_lock(&runq->mutex);
	runq->thread = pthread_self();

	for (;;) {
		struct tasklet *t = runq->head;
		if (!t)
			break;

		run_queue_remove(runq, t);
		runq->current = t;
		runq->current_requeue = runq->current_stopped = FALSE;
		t->waited = FALSE;

		if (mutex_transfer(&runq->mutex, t->mutex)) {
			t->handler(t->data);
			mutex_lock(&runq->mutex);
		}

		if (runq->current == t) {
			if (!runq->current_requeue) {
				/* Detect dangling tasklets that are
				   not on a waitlist and were not
				   explicitly stopped. */
				if (!runq->current_stopped) {
					assert(t->waited);
					assert(t->wait);
				}

				t->runq = NULL;
			}
			else {
				run_queue_enqueue(runq, t);
			}
		}

		if (runq->waiting) {
			runq->waiting = FALSE;
			cond_broadcast(&runq->cond);
		}
	}

	runq->current = NULL;
	mutex_unlock(&runq->mutex);
}

void tasklet_stop(struct tasklet *t)
{
	mutex_assert_held(t->mutex);
	tasklet_unwait(t);

	for (;;) {
		struct run_queue *runq = t->runq;
		if (runq == NULL)
			break;

		mutex_lock(&runq->mutex);

		if (t->runq == runq) {
			if (runq->current != t) {
				run_queue_remove(runq, t);
				t->runq = NULL;
			}
			else {
				runq->current_requeue = FALSE;
				runq->current_stopped = TRUE;

				if (pthread_self() != runq->thread) {
					mutex_veto_transfer(t->mutex);

					/* Wait until the tasklet is done */
					runq->waiting = TRUE;

					do
						cond_wait(&runq->cond,
							  &runq->mutex);
					while (runq->current == t);
				}
			}

			mutex_unlock(&runq->mutex);
			break;
		}

		mutex_unlock(&runq->mutex);
	}
}

void tasklet_fini(struct tasklet *t)
{
	mutex_assert_held(t->mutex);
	tasklet_unwait(t);

	for (;;) {
		struct run_queue *runq = t->runq;
		if (runq == NULL)
			break;

		mutex_lock(&runq->mutex);

		if (t->runq == runq) {
			if (runq->current != t) {
				run_queue_remove(runq, t);
				t->runq = NULL;
			}
			else if (pthread_self() == runq->thread) {
				runq->current = NULL;
			}
			else {
				mutex_veto_transfer(t->mutex);

				/* Wait until the tasklet is done */
				runq->waiting = TRUE;

				do
					cond_wait(&runq->cond, &runq->mutex);
				while (runq->current == t);
			}

			mutex_unlock(&runq->mutex);
			break;
		}

		mutex_unlock(&runq->mutex);
	}

	t->mutex = NULL;
	t->handler = NULL;
	t->data = NULL;
	mutex_fini(&t->wait_mutex);
}

void run_queue_thread_run(void)
{
	run_queue_run(thread_run_queue());
}
