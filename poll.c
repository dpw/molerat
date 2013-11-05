#include <poll.h>
#include <assert.h>
#include <signal.h>

#include "thread.h"
#include "tasklet.h"
#include "poll.h"
#include "application.h"

struct poll {
	struct mutex mutex;

	enum { NONE, PROCESSING, POLLING } thread_state;
	bool_t thread_stopping;
	struct thread thread;

	/* Non-null if there are watched_fd_infos that need syncing
	   into pollfds. */
	struct watched_fd *updates;
};

struct poll *poll_create(void)
{
	struct poll *p = xalloc(sizeof *p);

	mutex_init(&p->mutex);
	p->thread_state = NONE;
	p->thread_stopping = FALSE;
	p->updates = NULL;

	return p;
}

void poll_destroy(struct poll *p)
{
	mutex_lock(&p->mutex);

	if (p->thread_state != NONE) {
		p->thread_stopping = TRUE;
		p->thread_state = PROCESSING;
		thread_signal(thread_get_handle(&p->thread), PRIVATE_SIGNAL);
		mutex_unlock(&p->mutex);
		thread_fini(&p->thread);
		mutex_lock(&p->mutex);
	}

	assert(!p->updates);

	mutex_unlock_fini(&p->mutex);
	free(p);
}

struct watched_fd {
	struct poll *poll;

	/* The fd, or -1 if the client called watched_fd_destroy went
	   away. */
	int fd;
	short interest;

	watched_fd_handler_t handler;
	void *data;

	/* The slot in pollfds, or -1 if we don't have a slot. */
	long slot;

	/* Prev is non-NULL if this entry was updated and needs to be
	   synced into pollfds. */
	struct watched_fd *prev;
	struct watched_fd *next;
};

struct watched_fd *watched_fd_create(struct poll *poll, int fd,
				     watched_fd_handler_t handler, void *data)
{
	struct watched_fd *w;

	assert(fd >= 0);

	w = xalloc(sizeof *w);
	w->poll = poll;
	w->fd = fd;
	w->interest = 0;
	w->handler = handler;
	w->data = data;
	w->slot = -1;
	w->prev = NULL;

	return w;
}

static void poll_thread(void *v_p);

static void updated(struct watched_fd *w)
{
	struct poll *p;

	if (w->prev)
		/* Already on the updates list */
		return;

	p = w->poll;
	if (p->updates) {
		struct watched_fd *head = p->updates;
		struct watched_fd *tail = head->prev;

		w->prev = tail;
		w->next = head;
		head->prev = tail->next = w;
	}
	else {
		p->updates = w->prev = w->next = w;
	}

	/* Poke the poll thread */
	if (p->thread_state != PROCESSING) {
		if (p->thread_state == NONE)
			thread_init(&p->thread, poll_thread, p);
		else
			thread_signal(thread_get_handle(&p->thread),
				      PRIVATE_SIGNAL);

		p->thread_state = PROCESSING;
	}
}

void watched_fd_destroy(struct watched_fd *w)
{
	struct poll *p = w->poll;

	mutex_lock(&p->mutex);

	if (w->slot < 0) {
		struct poll *p = w->poll;

		/* Remove w from the updates list */
		if (w->prev) {
			if (w->prev == w) {
				p->updates = NULL;
			}
			else {
				w->next->prev = w->prev;
				w->prev->next = w->next;
				if (p->updates == w)
					p->updates = w->next;
			}
		}

		free(w);
	}
	else {
		w->fd = -1;
		w->interest = 0;
		updated(w);
	}

	mutex_unlock(&p->mutex);
}

void watched_fd_set_interest(struct watched_fd *w, short interest)
{
	mutex_lock(&w->poll->mutex);
	w->interest |= interest;
	updated(w);
	mutex_unlock(&w->poll->mutex);
}

struct pollfds {
	struct pollfd *pollfds;
	struct watched_fd **watched_fds;
	size_t size;
	size_t used;
};

static void add_pollfd(struct pollfds *pollfds, struct watched_fd *w)
{
	size_t slot = pollfds->used++;
	size_t sz = pollfds->size;

	if (slot == sz) {
		/* Grow pollfds */
		pollfds->size = sz *= 2;
		pollfds->pollfds = xrealloc(pollfds->pollfds,
					    sz * sizeof *pollfds->pollfds);
		pollfds->watched_fds = xrealloc(pollfds->watched_fds,
					    sz * sizeof *pollfds->watched_fds);
	}

	pollfds->pollfds[slot].fd = w->fd;
	pollfds->pollfds[slot].events = w->interest;
	pollfds->watched_fds[slot] = w;
	w->slot = slot;
}

static void remove_pollfd(struct pollfds *pollfds, struct watched_fd *w)
{
	long slot = w->slot;

	/* Copy the last pollfd over the one to be deleted */
	pollfds->used--;
	pollfds->pollfds[slot] = pollfds->pollfds[pollfds->used];
	pollfds->watched_fds[slot] = pollfds->watched_fds[pollfds->used];
	pollfds->watched_fds[slot]->slot = slot;

	if (w->fd < 0)
		free(w);
	else
		w->slot = -1;
}

static void apply_updates(struct poll *p, struct pollfds *pollfds)
{
	struct watched_fd *head = p->updates;
	struct watched_fd *w, *next;

	if (!head)
		return;

	w = head;
	do {
		w->prev = NULL;
		next = w->next;

		if (w->slot < 0) {
			if (w->interest)
				add_pollfd(pollfds, w);
		}
		else if (w->interest) {
			pollfds->pollfds[w->slot].events = w->interest;
		}
		else {
			remove_pollfd(pollfds, w);
		}

		w = next;
	} while (w != head);

	p->updates = NULL;
}

static void dispatch_events(struct pollfds *pollfds)
{
	size_t i;

	for (i = 0; i < pollfds->used; i++) {
		struct pollfd *pollfd = &pollfds->pollfds[i];
		struct watched_fd *w = pollfds->watched_fds[i];

		/* Dangling watched_fds will be cleaned up in the
		   next apply_updates pass */
		if (!pollfd->revents || w->fd < 0)
			continue;

		w->interest = pollfd->events
			= w->handler(w->data, pollfd->revents, w->interest);

		if (w->interest == 0)
			/* Add an update to remove this pollfd */
			updated(w);
	}
}

static void poll_thread(void *v_p)
{
	struct poll *p = v_p;
	struct run_queue *runq = run_queue_create();
	sigset_t sigmask;
	struct pollfds pollfds;

	application_assert_prepared();

	pollfds.used = 0;
	pollfds.size = 10;
	pollfds.pollfds = xalloc(pollfds.size * sizeof *pollfds.pollfds);
	pollfds.watched_fds
		= xalloc(pollfds.size * sizeof *pollfds.watched_fds);

	run_queue_target(runq);

	check_pthreads("pthread_sigmask",
		       pthread_sigmask(SIG_SETMASK, NULL, &sigmask));
	sigdelset(&sigmask, PRIVATE_SIGNAL);

	for (;;) {
		mutex_lock(&p->mutex);
		apply_updates(p, &pollfds);
		if (p->thread_stopping)
			break;

		p->thread_state = POLLING;
		mutex_unlock(&p->mutex);

		if (ppoll(pollfds.pollfds, pollfds.used, NULL, &sigmask) < 0) {
			if (errno != EINTR)
				check_syscall("ppoll", 0);

			continue;
		}

		mutex_lock(&p->mutex);
		p->thread_state = PROCESSING;
		dispatch_events(&pollfds);
		mutex_unlock(&p->mutex);

		run_queue_run(runq, FALSE);
	}

	mutex_unlock(&p->mutex);
	assert(!pollfds.used);
	free(pollfds.pollfds);
	free(pollfds.watched_fds);
}

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
