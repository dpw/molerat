#include <unistd.h>
#include <sys/epoll.h>
#include <signal.h>

#include "thread.h"
#include "tasklet.h"
#include "poll.h"
#include "application.h"

/* This code to convert between the system event bitsets and our
   bitsets looks cumbersome, but compiles down to the simple bitwise
   instructions you would expect. */

#define ASSERT_SINGLE_BIT(bit) typedef char assert_single_bit_##bit[!((bit)&((bit)-1))*2-1]
#define TRANSLATE_BIT(val, from, to) ((unsigned int)(from) > (unsigned int)(to) ? ((val) & (from)) / ((from) / (to)) : ((val) & (from)) * ((to) / (from)))

/* Translate an event set to the system representation */
static unsigned int events_to_system(unsigned int events)
{
	ASSERT_SINGLE_BIT(EPOLLIN);
	ASSERT_SINGLE_BIT(EPOLLOUT);
	ASSERT_SINGLE_BIT(EPOLLERR);

	return TRANSLATE_BIT(events, POLL_EVENT_IN, EPOLLIN)
		| TRANSLATE_BIT(events, POLL_EVENT_OUT, EPOLLOUT)
		| TRANSLATE_BIT(events, POLL_EVENT_ERR, EPOLLERR);
}

/* Translate an event set from the system representation */
static unsigned int events_from_system(unsigned int events)
{
	return TRANSLATE_BIT(events, EPOLLIN, POLL_EVENT_IN)
		| TRANSLATE_BIT(events, EPOLLOUT, POLL_EVENT_OUT)
		| TRANSLATE_BIT(events, EPOLLERR, POLL_EVENT_ERR);
}

struct poll {
	struct mutex mutex;
	struct thread thread;
	int epfd;
	bool_t thread_woken;
	bool_t thread_stopping;
	size_t watched_fd_count;
	size_t gone_count;
	struct watched_fd *gone;
};

struct watched_fd {
	struct poll *poll;
	int fd;
	short interest;
	bool_t added;
	watched_fd_handler_t handler;
	void *data;
	struct watched_fd *next;
};

static void poll_thread(void *v_p);

struct poll *poll_create(void)
{
	struct poll *p = xalloc(sizeof *p);

	mutex_init(&p->mutex);
	p->epfd = epoll_create(42);
	check_syscall("epoll_create", p->epfd >= 0);
	thread_init(&p->thread, poll_thread, p);
	p->thread_woken = TRUE;
	p->thread_stopping = FALSE;
	p->watched_fd_count = 0;
	p->gone_count = 0;
	p->gone = NULL;

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
	mutex_lock(&p->mutex);
	if (!p->thread_stopping) {
		p->thread_stopping = p->thread_woken = TRUE;
		thread_signal(thread_get_handle(&p->thread), PRIVATE_SIGNAL);
		mutex_unlock(&p->mutex);
		thread_fini(&p->thread);
		mutex_lock(&p->mutex);
	}

	assert(!p->watched_fd_count);
	free_gone_watched_fds(p);
	check_syscall("close", !close(p->epfd));
	mutex_unlock_fini(&p->mutex);
	free(p);
}

struct watched_fd *watched_fd_create(struct poll *poll, int fd,
				     watched_fd_handler_t handler, void *data)
{
	struct watched_fd *w;

	assert(fd >= 0);

	w = xalloc(sizeof *w);
	w->poll = poll;
	w->fd = fd;
	w->interest = 0;
	w->added = FALSE;
	w->handler = handler;
	w->data = data;

	mutex_lock(&poll->mutex);
	poll->watched_fd_count++;
	mutex_unlock(&poll->mutex);

	return w;
}

void watched_fd_set_interest(struct watched_fd *w, short interest)
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
			return;

		op = EPOLL_CTL_MOD;
	}

	ee.events = events_to_system(interest) | EPOLLET;
	ee.data.ptr = w;
	check_syscall("epoll_ctl", !epoll_ctl(w->poll->epfd, op, w->fd, &ee));
	w->added = TRUE;
	w->interest = interest;
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

	mutex_lock(&poll->mutex);
	poll->watched_fd_count--;
	w->next = poll->gone;
	poll->gone = w;

	/* Only wake the thread up to free watched_fds in batches */
	if (++poll->gone_count == 100) {
		poll->thread_woken = TRUE;
		thread_signal(thread_get_handle(&poll->thread), PRIVATE_SIGNAL);
	}

	mutex_unlock(&poll->mutex);
}

static void poll_thread(void *v_p)
{
	struct poll *p = v_p;
	struct run_queue *runq = run_queue_create();
	sigset_t sigmask;
	struct epoll_event ee[100];

	application_assert_prepared();
	run_queue_target(runq);

	check_pthreads("pthread_sigmask",
		       pthread_sigmask(SIG_SETMASK, NULL, &sigmask));
	sigdelset(&sigmask, PRIVATE_SIGNAL);

	for (;;) {
		int i, count;

		mutex_lock(&p->mutex);
		if (p->thread_stopping)
			break;

		free_gone_watched_fds(p);
		p->thread_woken = FALSE;
		mutex_unlock(&p->mutex);

		count = epoll_pwait(p->epfd, ee, 100, -1, &sigmask);
		if (count < 0) {
			if (errno == EINTR)
				continue;

			check_syscall("epoll_pwoll", FALSE);
		}

		mutex_lock(&p->mutex);

		for (i = 0; i < count; i++) {
			struct watched_fd *w = ee[i].data.ptr;
			if (w->fd >= 0)
				w->handler(w->data,
					   events_from_system(ee[i].events));
		}

		mutex_unlock(&p->mutex);

		run_queue_run(runq, FALSE);
	}

	mutex_unlock(&p->mutex);
}
