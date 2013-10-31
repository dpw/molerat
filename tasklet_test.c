#include "tasklet.h"

struct test_tasklet {
	struct mutex mutex;
	struct tasklet tasklet;
	struct wait_list *sema;
	unsigned int got;
};

void test_tasklet_wait(void *v_tt)
{
	struct test_tasklet *tt = v_tt;

	while (wait_list_down(tt->sema, 1, &tt->tasklet))
		tt->got++;

	mutex_unlock(&tt->mutex);
}

struct test_tasklet *test_tasklet_create(struct wait_list *sema)
{
	struct test_tasklet *tt = xalloc(sizeof *tt);

	mutex_init(&tt->mutex);
	tasklet_init(&tt->tasklet, &tt->mutex, tt);
	tt->sema = sema;
	tt->got = 0;

	mutex_lock(&tt->mutex);
	tasklet_now(&tt->tasklet, test_tasklet_wait);

	return tt;
}

void test_tasklet_destroy(struct test_tasklet *tt)
{
	mutex_lock(&tt->mutex);
	tasklet_fini(&tt->tasklet);
	mutex_unlock_fini(&tt->mutex);
	free(tt);
}

static void test_wait_list(void)
{
	int count = 3;
	int i, total_got;
	struct test_tasklet **tts = xalloc(count * sizeof *tts);
	struct wait_list sema;

	wait_list_init(&sema, 0);

	for (i = 0; i < count; i++)
		tts[i] = test_tasklet_create(&sema);

	wait_list_broadcast(&sema);
	run_queue_thread_run();

	for (i = 0; i < count; i++) {
		wait_list_up(&sema, 2);
		run_queue_thread_run();
	}

	total_got = 0;
	for (i = 0; i < count; i++)
		total_got += tts[i]->got;

	assert(total_got == count * 2);

	for (i = 0; i < count; i++)
		test_tasklet_destroy(tts[i]);

	wait_list_fini(&sema);
	free(tts);
}

/* Wait a millisecond */
static void delay(void)
{
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 1000000;
        assert(!nanosleep(&ts, NULL));
}

struct trqw {
	struct mutex mutex;
	struct tasklet tasklet;
	bool_t *ran;
};

static void test_run_queue_waiting_done(void *v_t)
{
	struct trqw *t = v_t;

	*t->ran = TRUE;

	tasklet_fini(&t->tasklet);
	mutex_unlock_fini(&t->mutex);
	free(t);
}


static void test_run_queue_waiting_thread(void *v_ran)
{
	struct trqw *t = xalloc(sizeof *t);

	mutex_init(&t->mutex);
	tasklet_init(&t->tasklet, &t->mutex, t);
	t->ran = v_ran;

	delay();
	tasklet_later(&t->tasklet, test_run_queue_waiting_done);
}

static void test_run_queue_waiting(void)
{
	bool_t ran = FALSE;
	struct thread thr;

	thread_init(&thr, test_run_queue_waiting_thread, &ran);
	run_queue_thread_run_waiting();
	assert(ran);
	thread_fini(&thr);
}

int main(void)
{
	test_wait_list();
	test_run_queue_waiting();
        return 0;
}
