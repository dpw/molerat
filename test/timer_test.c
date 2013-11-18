#include <molerat/timer.h>
#include <molerat/application.h>

#define TIMER_COUNT 100

struct timer_test {
	struct mutex mutex;
	int remaining;
	struct t {
		struct timer_test *parent;
		struct tasklet tasklet;
		struct timer timer;
		bool_t done;
	} timers[TIMER_COUNT];
};

static void randomize_timer(struct timer *t)
{
	xtime_t delay = random() % (XTIME_SECOND / 2);
	timer_set_relative(t, delay - random() % (XTIME_SECOND / 10),
			   delay + random() % (XTIME_SECOND / 10));
}

static void timer_test_tasklet(void *v_t)
{
	struct t *t = v_t;
	int delta = 1;
	xtime_t now;
	struct t *other;

	if (!timer_wait(&t->timer, &t->tasklet))
		goto out;

	assert(!t->done);
	t->done = TRUE;

	now = time_now();
	assert(now >= t->timer.earliest);
	/* Allow 100ms fuzz when checking when a timer fired */
	assert(now <= t->timer.latest + XTIME_SECOND / 10);

	/* Cancel a timer at random. */
	other = t->parent->timers + rand() % TIMER_COUNT;
	timer_cancel(&other->timer);
	if (!other->done) {
		delta += 1;
		other->done = TRUE;
	}

	/* Delay a timer at random */
	other = t->parent->timers + rand() % TIMER_COUNT;
	if (!other->done)
		randomize_timer(&other->timer);

	tasklet_stop(&t->tasklet);
	if (!(t->parent->remaining -= delta))
		/* All timers have fired or been cancelled */
		application_stop();

 out:
	mutex_unlock(&t->parent->mutex);
}

static void timer_test(void)
{
	struct timer_test tt;
	int i;
	bool_t interrupted;

	mutex_init(&tt.mutex);
	tt.remaining = TIMER_COUNT;

	mutex_lock(&tt.mutex);

	for (i = 0; i < TIMER_COUNT; i++) {
		struct t *t = &tt.timers[i];
		t->parent = &tt;
		tasklet_init(&t->tasklet, &tt.mutex, t);
		timer_init(&t->timer);
		t->done = FALSE;
		randomize_timer(&t->timer);
		tasklet_later(&t->tasklet, timer_test_tasklet);
	}

	mutex_unlock(&tt.mutex);
	interrupted = !application_run();
	mutex_lock(&tt.mutex);
	assert(interrupted || !tt.remaining);

	for (i = 0; i < TIMER_COUNT; i++) {
		struct t *t = &tt.timers[i];
		timer_fini(&t->timer);
		tasklet_fini(&t->tasklet);
	}

	mutex_unlock_fini(&tt.mutex);
}

int main(void)
{
	application_prepare();
	timer_test();
	return 0;
}
