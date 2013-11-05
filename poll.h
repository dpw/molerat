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

struct poll *poll_create(void);
void poll_destroy(struct poll *p);

struct poll *poll_singleton(void);

struct watched_fd *watched_fd_create(struct poll *poll, int fd,
				     watched_fd_handler_t handler, void *data);
void watched_fd_destroy(struct watched_fd *w);

/* This ors the given event bits into the interest bits. */
void watched_fd_set_interest(struct watched_fd *w, short event);

#endif
