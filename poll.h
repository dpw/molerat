#ifndef MOLERAT_POLL_H
#define MOLERAT_POLL_H

/* You probablu shouldn't be using this directly!  socket.h provides a
 * higher-level interface onto sockets. */

/* For POLLIN, POLLOUT, etc. */
#include <poll.h>

/* A handler takes the event bits actually recieved, and the exiting
 * interest set, and returns the new interest set. Note that the
 * handler gets called with the poll lock held. Be careful to avoid
 * deadlock! */

typedef short (*watched_fd_handler_t)(void *data, short got, short interest);

struct watched_fd {
	struct poll *poll;
	int fd;

	/* The sign of slot indicates what it refers to:
	 *
	 * >0: There is an entry in pollfds for the socket, at
	 * pollfds[slot-1].
	 *
	 * <0: There is an entry in pollfd_updates for the socket, at
	 * pollfds[~slot].  There may or may not be a pollfd entry
	 * too (indicated by the poll_slot of the pollfd_update)
	 *
	 * 0: There is no pollfd or pollfd_update for this socket.
	 */
	int slot;

	watched_fd_handler_t handle_events;
	void *data;
};

struct poll *poll_create(void);
void poll_destroy(struct poll *p);

struct poll *poll_singleton(void);

void watched_fd_init(struct watched_fd *w, struct poll *poll, int fd,
		     watched_fd_handler_t handle_events, void *data);
void watched_fd_fini(struct watched_fd *w);

/* This ors the given event bits into the interest bits. */
void watched_fd_set_interest(struct watched_fd *w, short event);

#endif
