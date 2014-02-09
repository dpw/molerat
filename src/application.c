#include <signal.h>
#include <assert.h>

#include <molerat/base.h>
#include <molerat/thread.h>
#include <molerat/application.h>

static struct mutex app_mutex = MUTEX_INITIALIZER;
static bool_t app_prepared;
static bool_t sigint_blocked;
static int app_running;
static int app_runs;
static int app_stops;
static thread_handle_t app_main_thread;

static void wakey_wakey(int sig)
{
	/* No action is needed: signal delivery will wake the thread. */
	(void)sig;
}

static void app_prepare(sigset_t *block)
{
	mutex_lock(&app_mutex);
	assert(!app_prepared);
	app_prepared = TRUE;
	mutex_unlock(&app_mutex);

	check_pthreads("pthread_sigmask",
		       pthread_sigmask(SIG_BLOCK, block, NULL));

	check_syscall("signal", signal(PRIVATE_SIGNAL, wakey_wakey) != SIG_ERR);
	check_syscall("signal", signal(SIGPIPE, SIG_IGN) != SIG_ERR);
}

void application_prepare(void)
{
	sigset_t block;

	sigemptyset(&block);
	sigaddset(&block, PRIVATE_SIGNAL);
	sigaddset(&block, SIGINT);

	app_prepare(&block);
	sigint_blocked = TRUE;
}

void application_prepare_test(void)
{
	sigset_t block;

	sigemptyset(&block);
	sigaddset(&block, PRIVATE_SIGNAL);

	app_prepare(&block);
}

void application_assert_prepared(void)
{
	mutex_lock(&app_mutex);
	assert(app_prepared);
	mutex_unlock(&app_mutex);
}

bool_t application_run(void)
{
	int run;
	sigset_t sigs;
	int sig;
	bool_t res = TRUE;

	sigemptyset(&sigs);
	sigaddset(&sigs, PRIVATE_SIGNAL);

	if (sigint_blocked)
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
