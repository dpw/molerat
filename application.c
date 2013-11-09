#include <signal.h>
#include <assert.h>

#include "base.h"
#include "thread.h"
#include "application.h"

static struct mutex app_mutex = MUTEX_INITIALIZER;
static int app_running;
static int app_runs;
static int app_stops;
static thread_handle_t app_main_thread;

static void wakey_wakey(int sig)
{
	/* No action is needed: signal delivery will wake the thread. */
	(void)sig;
}

void application_prepare(void)
{
	sigset_t block;

	sigemptyset(&block);
	sigaddset(&block, PRIVATE_SIGNAL);
	sigaddset(&block, SIGINT);
	check_pthreads("pthread_sigmask",
		       pthread_sigmask(SIG_BLOCK, &block, NULL));

	check_syscall("signal", signal(PRIVATE_SIGNAL, wakey_wakey) != SIG_ERR);
	check_syscall("signal", signal(SIGPIPE, SIG_IGN) != SIG_ERR);
}

void application_assert_prepared(void)
{
	sigset_t blocked;
	struct sigaction sa;

	check_pthreads("pthread_sigmask",
		       pthread_sigmask(SIG_SETMASK, NULL, &blocked));
	assert(sigismember(&blocked, PRIVATE_SIGNAL));
	assert(sigismember(&blocked, SIGINT));

	check_syscall("sigaction", !sigaction(PRIVATE_SIGNAL, NULL, &sa));
	assert(sa.sa_handler == wakey_wakey);
}

bool_t application_run(void)
{
	int run;
	sigset_t sigs;
	int sig;
	bool_t res = TRUE;

	sigemptyset(&sigs);
	sigaddset(&sigs, PRIVATE_SIGNAL);
	sigaddset(&sigs, SIGINT);

	mutex_lock(&app_mutex);
	run = app_runs++;

	if (!app_running++)
		app_main_thread = thread_handle_current();

	while (run >= app_stops) {
		mutex_unlock(&app_mutex);
		check_pthreads("sigwait", sigwait(&sigs, &sig));
		mutex_lock(&app_mutex);

		if (sig == SIGINT) {
			res = FALSE;
			break;
		}
	}

	app_running--;
	mutex_unlock(&app_mutex);
	return res;
}

void application_stop(void)
{
	mutex_lock(&app_mutex);

	app_stops++;
	if (app_running)
		thread_signal(app_main_thread, PRIVATE_SIGNAL);

	mutex_unlock(&app_mutex);
}
