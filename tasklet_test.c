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

int main(void)
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

        return 0;
}
