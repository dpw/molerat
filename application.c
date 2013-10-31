#include "tasklet.h"
#include "application.h"

static bool_t stop;

void application_run(void)
{
	stop = 0;
	/* old_sigint = signal(SIGINT, sigint_handler); */

	while (!stop)
		run_queue_thread_run_waiting();

	/* signal(SIGINT, old_sigint); */
}

void application_stop(void)
{
	stop = 1;
}
