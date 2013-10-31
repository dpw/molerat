#include <poll.h>
#include <assert.h>

#include "thread.h"
#include "poll.h"

struct poll {
	struct mutex mutex;

	/* pollfs is the array of pollfds that gets passed to poll(2).
	 *
	 * We don't want to modify pollfds while poll is being called,
	 * or while the events are being dispatched (which gets
	 * complicated if pollfd entries are moving around).  Instead,
	 * updates to pollfds are gathered in pollfd_updates, and
	 * pollfds only gets modified within apply_updates.
	 *
	 * watched_fds records the sockets corresponding to pollfds
	 * entries.
	 */
	struct pollfd *pollfds;
	struct watched_fd **watched_fds;
	int pollfds_size;
	int pollfds_used;

	struct pollfd_update *pollfd_updates;
	int pollfd_updates_size;
	int pollfd_updates_used;
};

struct pollfd_update {
	/* The index of the entry in pollfds to which this update
	   applies, or -1 if it is new. */
	int poll_slot;
	short events;
	struct watched_fd *watched_fd;
};

struct poll *poll_create(void)
{
	struct poll *p = xalloc(sizeof *p);

	mutex_init(&p->mutex);
	p->pollfds = NULL;
	p->watched_fds = NULL;
	p->pollfds_size = p->pollfds_used = 0;
	p->pollfd_updates = NULL;
	p->pollfd_updates_size = p->pollfd_updates_used = 0;

	return p;
}

void poll_destroy(struct poll *p)
{
	int i;

	for (i = 0; i < p->pollfds_used; i++)
		assert(!p->watched_fds[i]);

	for (i = 0; i < p->pollfd_updates_used; i++)
		assert(!p->pollfd_updates[i].watched_fd);

	free(p->pollfds);
	free(p->watched_fds);
	free(p->pollfd_updates);
	mutex_fini(&p->mutex);
	free(p);
}

void watched_fd_init(struct watched_fd *w, struct poll *poll, int fd,
		     watched_fd_handler_t handle_events, void *data)
{
	w->poll = poll;
	w->fd = fd;
	w->slot = 0;
	w->handle_events = handle_events;
	w->data = data;
}

/* Allocate a new pollfd_update and return its slot. */
static int add_update(struct poll *p, int poll_slot, short events,
		      struct watched_fd *w)
{
	int slot = p->pollfd_updates_used++;
	int sz = p->pollfd_updates_size;
	struct pollfd_update *update;

	if (slot == sz) {
		/* Grow pollfd_updates */
		p->pollfd_updates_size = sz = sz * 2 + 10;
		p->pollfd_updates = xrealloc(p->pollfd_updates,
					     sz * sizeof(struct pollfd_update));
	}

	update = &p->pollfd_updates[slot];
	update->poll_slot = poll_slot;
	update->events = events;
	update->watched_fd = w;

	return slot;
}

void watched_fd_fini(struct watched_fd *w)
{
	struct poll *p = w->poll;
	mutex_lock(&p->mutex);

	if (w->slot < 0) {
		/* Mark the existing update so that its effect is to
		   release the pollfd */
		struct pollfd_update *u = &p->pollfd_updates[~w->slot];

		u->watched_fd = NULL;
		if (u->poll_slot >= 0)
			p->watched_fds[u->poll_slot] = NULL;
	}
	else if (w->slot > 0) {
		/* Allocate a new update to release the pollfd */
		p->watched_fds[w->slot - 1] = NULL;
		add_update(p, w->slot - 1, 0, NULL);
	}

	w->fd = -1;
	mutex_unlock(&p->mutex);
}

void watched_fd_set_interest(struct watched_fd *w, short events)
{
	struct poll *p = w->poll;
	mutex_lock(&p->mutex);

	if (w->slot >= 0) {
		if (w->slot > 0)
			events |= p->pollfds[w->slot - 1].events;

		w->slot = ~add_update(p, w->slot - 1, events, w);
	}
	else {
		/* Update the existing update slot */
		p->pollfd_updates[~w->slot].events |= events;
	}

	mutex_unlock(&p->mutex);
}

static void add_pollfd(struct poll *p, struct watched_fd *w,
		       short events)
{
	int slot = p->pollfds_used++;
	int sz = p->pollfds_size;

	if (slot == sz) {
		/* Grow pollfds */
		p->pollfds_size = sz = sz * 2 + 10;
		p->pollfds = xrealloc(p->pollfds, sz * sizeof(struct pollfd));
		p->watched_fds = xrealloc(p->watched_fds,
					  sz * sizeof(struct watched_fd *));
	}

	p->pollfds[slot].fd = w->fd;
	p->pollfds[slot].events = events;
	p->watched_fds[slot] = w;
	w->slot = slot + 1;
}

static void delete_pollfd(struct poll *p, int slot)
{
	int fd;

	/* Copy the last pollfd over the one to be deleted */
	p->pollfds_used--;
	p->pollfds[slot] = p->pollfds[p->pollfds_used];
	p->watched_fds[slot] = p->watched_fds[p->pollfds_used];

	/* Now fix up the slot reference for the moved pollfd, which
	   could be in the common_socket or in the pollfd_update */
	fd = p->pollfds[slot].fd;
	if (fd >= 0)
		p->watched_fds[slot]->slot = slot + 1;
	else
		p->pollfd_updates[~fd].poll_slot = slot;
}

static void apply_updates(struct poll *p)
{
	int i;

	/* We abuse the 'fd' field in pollfds during the update:
	 * Because we might shuffle the entries in f->pollfds below,
	 * we need to keep the 'slot' fields in the pollfd_updates in
	 * sync.  But that requires a way to get from a pollfd to the
	 * pollfd_update.  We temporarily stash negative values in the
	 * 'fd' to provide that path. */
	for (i = 0; i < p->pollfd_updates_used; i++) {
		int poll_slot = p->pollfd_updates[i].poll_slot;
		if (poll_slot >= 0)
			p->pollfds[poll_slot].fd = ~i;
	}

	for (i = 0; i < p->pollfd_updates_used; i++) {
		struct pollfd_update *u = &p->pollfd_updates[i];
		if (u->poll_slot >= 0) {
			/* The case where a pollfd already exists */
			if (u->events) {
				struct pollfd *pollfd
					= &p->pollfds[u->poll_slot];
				pollfd->events = u->events;
				pollfd->fd = u->watched_fd->fd;
				u->watched_fd->slot = u->poll_slot + 1;
			}
			else {
				delete_pollfd(p, u->poll_slot);
				if (u->watched_fd)
					u->watched_fd->slot = 0;
			}
		}
		else if (u->events) {
			/* The is no existing pollfd for this socket. */
			add_pollfd(p, u->watched_fd, u->events);
		}

	}

	p->pollfd_updates_used = 0;
}

void poll_poll(struct poll *p, struct error *e)
{
	struct pollfd *pollfds;
	int pollfds_used, i;

	mutex_lock(&p->mutex);
	apply_updates(p);
	pollfds = p->pollfds;
	pollfds_used = p->pollfds_used;
	mutex_unlock(&p->mutex);

	if (poll(pollfds, pollfds_used, -1) < 0) {
		if (errno != EINTR)
			error_errno(e, "poll");

		return;
	}

	mutex_lock(&p->mutex);

	for (i = 0; i < pollfds_used; i++) {
		if (pollfds[i].revents) {
			struct watched_fd *w = p->watched_fds[i];
			if (w)
				pollfds[i].events = w->handle_events(w->data,
							     pollfds[i].revents,
							     pollfds[i].events);
		}
	}

	mutex_unlock(&p->mutex);
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
