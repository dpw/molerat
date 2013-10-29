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
};

static struct test *test_create(unsigned int queue_size)
{
	struct test *t = xalloc(sizeof *t);
	mutex_init(&t->mutex);
	queue_init(&t->queue, queue_size);

	tasklet_init(&t->producer, &t->mutex, t);
	t->producer_count = 0;
	t->producer_blocked = 0;

	tasklet_init(&t->consumer, &t->mutex, t);
	t->consumer_count = 0;
	t->consumer_blocked = 0;

	return t;
}

static void test_destroy(struct test *t)
{
	mutex_lock(&t->mutex);
	tasklet_fini(&t->producer);
	tasklet_fini(&t->consumer);
	queue_fini(&t->queue);
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
			t->producer_blocked = 1;
			mutex_unlock(&t->mutex);
			free(item);
			return;
		}

		t->producer_count++;
	}
}

static void start_producer(struct test *t)
{
	mutex_lock(&t->mutex);
 	t->producer_blocked = 0;
	tasklet_now(&t->producer, producer);
}

static void pause_producer(struct test *t)
{
	mutex_lock(&t->mutex);
	tasklet_stop(&t->producer);
	mutex_unlock(&t->mutex);
}

static void consumer(void *v_t)
{
	struct test *t = v_t;

	for (;;) {
		int *item = queue_shift(&t->queue, &t->consumer);
		if (!item) {
			t->consumer_blocked = 1;
			mutex_unlock(&t->mutex);
			return;
		}

		assert(*item == t->consumer_count++);
		free(item);
	}
}

static void start_consumer(struct test *t)
{
	mutex_lock(&t->mutex);
	t->consumer_blocked = 0;
	tasklet_now(&t->consumer, consumer);
}

int main(void)
{
	struct test *t = test_create(100);

	/* Run the producer until the queue is full */
	start_producer(t);
	while (!t->producer_blocked)
		run_queue_thread_run();
	pause_producer(t);
	assert(t->producer_count == 100);

	/* Run the consumer until the queue is empty */
	start_consumer(t);
	while (!t->consumer_blocked)
		run_queue_thread_run();
	assert(t->consumer_count == 100);

	/* Run the producer and the consumer together */
	start_producer(t);
	while (t->producer_count < 200)
		run_queue_thread_run();

	/* Stop the producer, drain the queue */
	pause_producer(t);
	start_consumer(t);
	while (!t->consumer_blocked);
		run_queue_thread_run();
	assert(t->consumer_count == 200);

	test_destroy(t);
	return 0;
}
