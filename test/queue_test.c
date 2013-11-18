#include "queue.h"
#include "tasklet.h"

struct test {
	struct mutex mutex;
	struct queue queue;

	struct tasklet producer;
	int producer_count;
	bool_t producer_blocked;

	struct tasklet consumer;
	int consumer_count;
	bool_t consumer_blocked;

	struct cond cond;
};

static struct test *test_create(unsigned int queue_size)
{
	struct test *t = xalloc(sizeof *t);
	mutex_init(&t->mutex);
	cond_init(&t->cond);
	queue_init(&t->queue, queue_size);

	tasklet_init(&t->producer, &t->mutex, t);
	t->producer_count = 0;

	tasklet_init(&t->consumer, &t->mutex, t);
	t->consumer_count = 0;

	t->consumer_blocked = t->producer_blocked = FALSE;
	return t;
}

static void test_destroy(struct test *t)
{
	mutex_lock(&t->mutex);
	tasklet_fini(&t->producer);
	tasklet_fini(&t->consumer);
	queue_fini(&t->queue);
	cond_fini(&t->cond);
	mutex_unlock_fini(&t->mutex);
	free(t);
}

static void producer(void *v_t)
{
	struct test *t = v_t;

	for (;;) {
		int *item = xalloc(sizeof *item);
		*item = t->producer_count;
		if (!queue_push(&t->queue, item, &t->producer)) {
			t->producer_blocked = TRUE;
			cond_broadcast(&t->cond);
			mutex_unlock(&t->mutex);
			free(item);
			return;
		}

		t->producer_count++;

		/* Stop at 200 items */
		if (t->producer_count == 200) {
			cond_broadcast(&t->cond);
			tasklet_stop(&t->producer);
			mutex_unlock(&t->mutex);
			return;
		}
	}
}

static void consumer(void *v_t)
{
	struct test *t = v_t;

	for (;;) {
		int *item = queue_shift(&t->queue, &t->consumer);
		if (!item) {
			t->consumer_blocked = TRUE;
			cond_broadcast(&t->cond);
			mutex_unlock(&t->mutex);
			return;
		}

		assert(*item == t->consumer_count);
		free(item);
		t->consumer_count++;
	}
}

int main(void)
{
	struct test *t = test_create(100);

	mutex_lock(&t->mutex);

	/* Run the producer until the queue is full */
 	t->producer_blocked = FALSE;
	tasklet_later(&t->producer, producer);

	while (!t->producer_blocked)
		cond_wait(&t->cond, &t->mutex);

	tasklet_stop(&t->producer);
	assert(t->producer_count == 100);

	/* Run the consumer until the queue is empty */
	t->consumer_blocked = FALSE;
	tasklet_later(&t->consumer, consumer);

	while (!t->consumer_blocked)
		cond_wait(&t->cond, &t->mutex);

	assert(t->consumer_count == 100);

	/* Run the producer and the consumer together until 200 items
	   have been produced. */
 	t->producer_blocked = FALSE;
	t->consumer_blocked = FALSE;
	tasklet_later(&t->producer, producer);

	while (t->producer_count < 200)
		cond_wait(&t->cond, &t->mutex);

	while (!t->consumer_blocked)
		cond_wait(&t->cond, &t->mutex);

	assert(t->consumer_count == t->producer_count);

	mutex_unlock(&t->mutex);
	test_destroy(t);
	return 0;
}
