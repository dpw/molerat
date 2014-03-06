#include <unistd.h>
#include <sys/epoll.h>
#include <signal.h>

#include <molerat/base.h>
#include <molerat/application.h>

#include "poll.h"

/* This code to convert between the system event bitsets and our
   bitsets looks cumbersome, but compiles down to the simple bitwise
   instructions you would expect. */

#define ASSERT_SINGLE_BIT(bit) STATIC_ASSERT(!((bit) & ((bit) - 1)))
#define TRANSLATE_BIT(val, from, to) ((unsigned int)(from) > (unsigned int)(to) ? ((val) & (from)) / ((from) / (to)) : ((val) & (from)) * ((to) / (from)))

ASSERT_SINGLE_BIT(EPOLLIN);
ASSERT_SINGLE_BIT(EPOLLOUT);
ASSERT_SINGLE_BIT(EPOLLERR);

/* Translate an event set to the system representation */
static unsigned int events_to_system(poll_events_t events)
{
	return TRANSLATE_BIT(events, WATCHED_FD_IN, EPOLLIN)
		| TRANSLATE_BIT(events, WATCHED_FD_OUT, EPOLLOUT)
		| TRANSLATE_BIT(events, WATCHED_FD_ERR, EPOLLERR);
}

/* Translate an event set from the system representation */
static poll_events_t events_from_system(unsigned int events)
{
	return TRANSLATE_BIT(events, EPOLLIN, WATCHED_FD_IN)
		| TRANSLATE_BIT(events, EPOLLOUT, WATCHED_FD_OUT)
		| TRANSLATE_BIT(events, EPOLLERR, WATCHED_FD_ERR);
}

struct poll {
	struct poll_common common;
	sigset_t sigmask;
	int epfd;
	size_t watched_fd_count;
	size_t gone_count;
	struct watched_fd *gone;

	struct epoll_event ee[100];
	int ee_count;
};

struct watched_fd {
	struct poll *poll;
	int fd;
	poll_events_t interest;
	bool_t added;
	watched_fd_handler_t handler;
	void *data;
	struct watched_fd *next;
};

struct poll *poll_create(void)
{
	struct poll *p = xalloc(sizeof *p);

	p->epfd = epoll_create(42);
	check_syscall("epoll_create", p->epfd >= 0);
	p->watched_fd_count = 0;
	p->gone_count = 0;
	p->gone = NULL;
	poll_common_init(&p->common);

	return p;
}

static void free_gone_watched_fds(struct poll *p)
{
	while (p->gone) {
		struct watched_fd *next = p->gone->next;
		free(p->gone);
		p->gone = next;
	}

	p->gone_count = 0;
}

void poll_destroy(struct poll *p)
{
	mutex_lock(&p->common.mutex);
	poll_common_stop(&p->common);
	assert(!p->watched_fd_count);
	free_gone_watched_fds(p);
	check_syscall("close", !close(p->epfd));
	mutex_unlock_fini(&p->common.mutex);
	free(p);
}

struct watched_fd *watched_fd_create(int fd, watched_fd_handler_t handler,
				     void *data)
{
	struct watched_fd *w;
	struct poll *poll = poll_singleton();

	assert(fd >= 0);

	w = xalloc(sizeof *w);
	w->poll = poll;
	w->fd = fd;
	w->interest = 0;
	w->added = FALSE;
	w->handler = handler;
	w->data = data;

	mutex_lock(&poll->common.mutex);
	poll->watched_fd_count++;
	mutex_unlock(&poll->common.mutex);

	return w;
}

bool_t watched_fd_set_interest(struct watched_fd *w, poll_events_t interest,
			       struct error *err)
{
	int op = EPOLL_CTL_ADD;
	struct epoll_event ee;

	interest |= w->interest;

	if (w->added) {
		/* If we have already told epoll about our interest in
		these events, don't do so again.  The caller should
		already have done something to arm epoll (e.g. a read
		returning EAGAIN).  At some point we might need an API
		to say "I'm not interested in some events any more",
		but redundant events are not likely to be much of a
		problem. */
		if (w->interest == interest)
			return TRUE;

		op = EPOLL_CTL_MOD;
	}

	ee.events = events_to_system(interest) | EPOLLET;
	ee.data.ptr = w;

	if (unlikely(epoll_ctl(w->poll->epfd, op, w->fd, &ee))) {
		error_errno(err, "epoll_ctl");
		return FALSE;
	}

	w->added = TRUE;
	w->interest = interest;
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

void watched_fd_destroy(struct watched_fd *w)
{
	struct poll *poll = w->poll;
	struct epoll_event ee;

	if (w->added)
		check_syscall("epoll_ctl",
			      !epoll_ctl(w->poll->epfd, EPOLL_CTL_DEL, w->fd,
					 &ee));

	w->fd = -1;

	mutex_lock(&poll->common.mutex);
	poll->watched_fd_count--;
	w->next = poll->gone;
	poll->gone = w;

	/* Only wake the thread up to free watched_fds in batches */
	if (++poll->gone_count == 100)
		poll_common_wake(&poll->common);

	mutex_unlock(&poll->common.mutex);
}

void poll_thread_init(struct poll *p)
{
	check_pthreads("pthread_sigmask",
		       pthread_sigmask(SIG_SETMASK, NULL, &p->sigmask));
	sigdelset(&p->sigmask, PRIVATE_SIGNAL);
}

void poll_prepare(struct poll *p)
{
	free_gone_watched_fds(p);
}

void poll_poll(struct poll *p, xtime_t timeout)
{
	if (timeout > 0)
		timeout = xtime_to_ms(timeout);

	p->ee_count = epoll_pwait(p->epfd, p->ee, 100, timeout, &p->sigmask);
	if (p->ee_count < 0 && errno != EINTR)
		check_syscall("epoll_pwait", FALSE);
}

void poll_dispatch(struct poll *p)
{
	int i;
	int count = p->ee_count;

	for (i = 0; i < count; i++) {
		struct watched_fd *w = p->ee[i].data.ptr;
		if (w->fd >= 0)
			w->handler(w->data,
				   events_from_system(p->ee[i].events));
	}
}

void poll_wake(struct poll *p)
{
	thread_signal(thread_get_handle(&p->common.thread), PRIVATE_SIGNAL);
}
