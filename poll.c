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
	struct watched_fd_info *updates;
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

/* There is a watched_fd_info for an fd as long as either a watched_fd
   exists or there is an entry in pollfds. */
struct watched_fd_info {
	struct poll *poll;

	/* The fd, or -1 if the watched_fd went away. */
	int fd;
	short interest;

	watched_fd_handler_t handler;
	void *data;

	/* The slot in pollfds, or -1 if we don't have a slot. */
	long slot;

	/* Prev is non-NULL if this entry was updated and needs to be
	   synced into pollfds. */
	struct watched_fd_info *prev;
	struct watched_fd_info *next;
};

void watched_fd_init(struct watched_fd *w, struct poll *poll, int fd,
		     watched_fd_handler_t handler, void *data)
{
	struct watched_fd_info *info;

	assert(fd >= 0);

	w->info = info = xalloc(sizeof *info);
	info->poll = poll;
	info->fd = fd;
	info->interest = 0;
	info->handler = handler;
	info->data = data;
	info->slot = -1;
	info->prev = NULL;
}

static void poll_thread(void *v_p);

static void updated(struct watched_fd_info *info)
{
	struct poll *p;

	if (info->prev)
		/* Already on the updates list */
		return;

	p = info->poll;
	if (p->updates) {
		struct watched_fd_info *head = p->updates;
		struct watched_fd_info *tail = head->prev;

		info->prev = tail;
		info->next = head;
		head->prev = tail->next = info;
	}
	else {
		p->updates = info->prev = info->next = info;
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

void watched_fd_fini(struct watched_fd *w)
{
	struct watched_fd_info *info = w->info;
	struct poll *p = info->poll;

	mutex_lock(&p->mutex);

	if (info->slot < 0) {
		struct poll *p = info->poll;

		/* Remove info from the updates list */
		if (info->prev) {
			if (info->prev == info) {
				p->updates = NULL;
			}
			else {
				info->next->prev = info->prev;
				info->prev->next = info->next;
				if (p->updates == info)
					p->updates = info->next;
			}
		}

		free(info);
	}
	else {
		info->fd = -1;
		info->interest = 0;
		updated(info);
	}

	mutex_unlock(&p->mutex);
}

void watched_fd_set_interest(struct watched_fd *w, short interest)
{
	struct watched_fd_info *info = w->info;

	mutex_lock(&info->poll->mutex);
	info->interest |= interest;
	updated(info);
	mutex_unlock(&info->poll->mutex);
}

struct pollfds {
	struct pollfd *pollfds;
	struct watched_fd_info **infos;
	size_t size;
	size_t used;
};

static void add_pollfd(struct pollfds *pollfds, struct watched_fd_info *info)
{
	size_t slot = pollfds->used++;
	size_t sz = pollfds->size;

	if (slot == sz) {
		/* Grow pollfds */
		pollfds->size = sz *= 2;
		pollfds->pollfds = xrealloc(pollfds->pollfds,
					    sz * sizeof *pollfds->pollfds);
		pollfds->infos = xrealloc(pollfds->infos,
					  sz * sizeof *pollfds->infos);
	}

	pollfds->pollfds[slot].fd = info->fd;
	pollfds->pollfds[slot].events = info->interest;
	pollfds->infos[slot] = info;
	info->slot = slot;
}

static void remove_pollfd(struct pollfds *pollfds, struct watched_fd_info *info)
{
	long slot = info->slot;

	/* Copy the last pollfd over the one to be deleted */
	pollfds->used--;
	pollfds->pollfds[slot] = pollfds->pollfds[pollfds->used];
	pollfds->infos[slot] = pollfds->infos[pollfds->used];
	pollfds->infos[slot]->slot = slot;

	if (info->fd < 0)
		free(info);
	else
		info->slot = -1;
}

static void apply_updates(struct poll *p, struct pollfds *pollfds)
{
	struct watched_fd_info *head = p->updates;
	struct watched_fd_info *info, *next;

	if (!head)
		return;

	info = head;
	do {
		info->prev = NULL;
		next = info->next;

		if (info->slot < 0) {
			if (info->interest)
				add_pollfd(pollfds, info);
		}
		else if (info->interest) {
			pollfds->pollfds[info->slot].events = info->interest;
		}
		else {
			remove_pollfd(pollfds, info);
		}

		info = next;
	} while (info != head);

	p->updates = NULL;
}

static void dispatch_events(struct pollfds *pollfds)
{
	size_t i;

	for (i = 0; i < pollfds->used; i++) {
		struct pollfd *pollfd = &pollfds->pollfds[i];
		struct watched_fd_info *info = pollfds->infos[i];

		/* Dangling watched_fd_infos will be cleaned up in the
		   next apply_updates pass */
		if (!pollfd->revents || info->fd < 0)
			continue;

		info->interest = pollfd->events
			= info->handler(info->data, pollfd->revents,
					info->interest);

		if (info->interest == 0)
			/* Add an update to remove this pollfd */
			updated(info);
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
	pollfds.infos = xalloc(pollfds.size * sizeof *pollfds.infos);

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
	free(pollfds.infos);
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
