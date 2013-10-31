#include <assert.h>

#include "base.h"
#include "thread.h"
#include "application.h"

struct mutex app_mutex = MUTEX_INITIALIZER;
struct cond app_cond;
enum { NONE, RUNNING, STOP } app_state;

void application_run(void)
{
	mutex_lock(&app_mutex);
	assert(app_state == NONE);
	app_state = RUNNING;
	cond_init(&app_cond);

	do
		cond_wait(&app_cond, &app_mutex);
	while (app_state == RUNNING);

	cond_fini(&app_cond);
	app_state = NONE;
	mutex_unlock(&app_mutex);
}

void application_stop(void)
{
	mutex_lock(&app_mutex);

	if (app_state == RUNNING) {
		app_state = STOP;
		cond_signal(&app_cond);
	}

	mutex_unlock(&app_mutex);
}
