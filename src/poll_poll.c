#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include "poll.h"

/* This code to convert between the system event bitsets and our
   bitsets looks cumbersome, but compiles down to the simple bitwise
   instructions you would expect. */

#define ASSERT_SINGLE_BIT(bit) STATIC_ASSERT(!((bit) & ((bit) - 1)))
#define TRANSLATE_BIT(val, from, to) ((from) > (to) ? ((val) & (from)) / ((from) / (to)) : ((val) & (from)) * ((to) / (from)))

ASSERT_SINGLE_BIT(POLLIN);
ASSERT_SINGLE_BIT(POLLOUT);
ASSERT_SINGLE_BIT(POLLERR);

/* Translate an event set to the system representation */
static unsigned int events_to_system(poll_events_t events)
{
	return TRANSLATE_BIT(events, WATCHED_FD_IN, POLLIN)
		| TRANSLATE_BIT(events, WATCHED_FD_OUT, POLLOUT)
		| TRANSLATE_BIT(events, WATCHED_FD_ERR, POLLERR);
}

/* Translate an event set from the system representation */
static poll_events_t events_from_system(unsigned int events)
{
	return TRANSLATE_BIT(events, POLLIN, WATCHED_FD_IN)
		| TRANSLATE_BIT(events, POLLOUT, WATCHED_FD_OUT)
		| TRANSLATE_BIT(events, POLLERR, WATCHED_FD_ERR)
		| TRANSLATE_BIT(events, POLLHUP, WATCHED_FD_ERR);
}

struct poll {
	struct poll_common common;

	/* Non-null if there are watched_fd_infos that need syncing
	   into pollfds. */
	struct watched_fd *updates;

	struct pollfds {
		struct pollfd *pollfds;
		struct watched_fd **watched_fds;
		size_t size;
		size_t used;
	} pollfds;

	int pipefd[2];
	int poll_result;
};

struct poll *poll_create(void)
{
	struct poll *p = xalloc(sizeof *p);
	int i;

	p->updates = NULL;

	/* pollfds.pollfds[0] is used for reading from the wakeup pipe */
	p->pollfds.used = 1;
	p->pollfds.size = 10;
	p->pollfds.pollfds = xalloc(10 * sizeof *p->pollfds.pollfds);
	p->pollfds.watched_fds = xalloc(10 * sizeof *p->pollfds.watched_fds);

	check_syscall("pipe", !pipe(p->pipefd));

	for (i = 0; i < 1; i++)
		check_syscall("fcntl(..., F_SETFL, O_NONBLOCK)",
			      fcntl(p->pipefd[i], F_SETFL, O_NONBLOCK) >= 0);

	p->pollfds.pollfds[0].fd = p->pipefd[0];
	p->pollfds.pollfds[0].events = POLLIN;

	poll_common_init(&p->common);
	return p;
}

void poll_destroy(struct poll *p)
{
	mutex_lock(&p->common.mutex);
	poll_common_stop(&p->common);
	assert(!p->updates);
	assert(p->pollfds.used == 1);
	mutex_unlock_fini(&p->common.mutex);
	check_syscall("close", !close(p->pipefd[0]));
	check_syscall("close", !close(p->pipefd[1]));
	free(p->pollfds.pollfds);
	free(p->pollfds.watched_fds);
	free(p);
}

struct watched_fd {
	struct poll *poll;

	/* The fd, or -1 if the client called watched_fd_destroy went
	   away. */
	int fd;
	poll_events_t interest;

	watched_fd_handler_t handler;
	void *data;

	/* The slot in pollfds, or -1 if we don't have a slot. */
	long slot;

	/* Prev is non-NULL if this entry was updated and needs to be
	   synced into pollfds. */
	struct watched_fd *prev;
	struct watched_fd *next;
};

struct watched_fd *watched_fd_create(int fd, watched_fd_handler_t handler,
				     void *data)
{
	struct watched_fd *w;

	assert(fd >= 0);

	w = xalloc(sizeof *w);
	w->poll = poll_singleton();
	w->fd = fd;
	w->interest = 0;
	w->handler = handler;
	w->data = data;
	w->slot = -1;
	w->prev = NULL;

	return w;
}

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
	poll_common_wake(&p->common);
}

void watched_fd_destroy(struct watched_fd *w)
{
	struct poll *p = w->poll;

	mutex_lock(&p->common.mutex);

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

	mutex_unlock(&p->common.mutex);
}

bool_t watched_fd_set_interest(struct watched_fd *w, poll_events_t interest,
			       struct error *err)
{
	(void)err;

	mutex_lock(&w->poll->common.mutex);
	w->interest |= interest;
	updated(w);
	mutex_unlock(&w->poll->common.mutex);
	return TRUE;
}

void watched_fd_set_handler(struct watched_fd *w, watched_fd_handler_t handler,
			    void *data)
{
	mutex_lock(&w->poll->common.mutex);
	w->handler = handler;
	w->data = data;
	mutex_unlock(&w->poll->common.mutex);
}

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
	pollfds->pollfds[slot].events = events_to_system(w->interest);
	pollfds->pollfds[slot].revents = 0;
	pollfds->watched_fds[slot] = w;
	w->slot = slot;
}

static void remove_pollfd(struct pollfds *pollfds, size_t slot)
{
	struct watched_fd *w = pollfds->watched_fds[slot];

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

void poll_thread_init(struct poll *p)
{
	(void)p;
}

void poll_prepare(struct poll *p)
{
	struct pollfds *pollfds = &p->pollfds;
	struct watched_fd *head = p->updates;
	struct watched_fd *w, *next;

	p->poll_result = 0;
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
			pollfds->pollfds[w->slot].events
				= events_to_system(w->interest);
		}
		else {
			remove_pollfd(pollfds, w->slot);
		}

		w = next;
	} while (w != head);

	p->updates = NULL;
}

void poll_poll(struct poll *p, xtime_t timeout)
{
	p->poll_result = poll(p->pollfds.pollfds, p->pollfds.used,
			      timeout >= 0 ? (int)xtime_to_ms(timeout) : -1);
	if (p->poll_result < 0 && errno != EINTR)
		check_syscall("ppoll", FALSE);
}

void poll_dispatch(struct poll *p)
{
	struct pollfds *pollfds = &p->pollfds;
	size_t i;
	poll_events_t got;

	if (!p->poll_result)
		/* Poll timed out, revents fields were not set. */
		return;

	if (p->pollfds.pollfds[0].revents & POLLIN) {
		/* data on wakeup pipe */
		char c;
		while (read(p->pipefd[0], &c, 1) < 0 && errno == EINTR)
		/* loop */;
	}

	for (i = 1; i < pollfds->used;) {
		struct pollfd *pollfd = &pollfds->pollfds[i];
		struct watched_fd *w = pollfds->watched_fds[i];

		/* Dangling watched_fds will be cleaned up in the
		   next apply_updates pass */
		if (!pollfd->revents || w->fd < 0) {
			i++;
			continue;
		}

		got = events_from_system(pollfd->revents);
		w->interest &= ~got;
		w->handler(w->data, got);
		if (w->interest == 0)
			remove_pollfd(pollfds, i);
		else
			i++;
	}
}

void poll_wake(struct poll *p)
{
	char c = 0;
	while (write(p->pipefd[1], &c, 1) < 0 && errno == EINTR)
		/* loop */;
}
