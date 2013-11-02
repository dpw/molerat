#include <assert.h>

#include "base.h"
#include "thread.h"
#include "application.h"

static struct mutex app_mutex = MUTEX_INITIALIZER;
static int app_running;
static int app_runs;
static int app_stops;
static struct cond app_cond;

void application_run(void)
{
	int run;

	mutex_lock(&app_mutex);
	run = app_runs++;

	if (!app_running++)
		cond_init(&app_cond);

	while (run >= app_stops)
		cond_wait(&app_cond, &app_mutex);

	if (!--app_running)
		cond_fini(&app_cond);

	mutex_unlock(&app_mutex);
}

void application_stop(void)
{
	mutex_lock(&app_mutex);

	app_stops++;
	if (app_running)
		cond_signal(&app_cond);

	mutex_unlock(&app_mutex);
}
