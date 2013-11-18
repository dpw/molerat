#include <string.h>
#include <stdio.h>

#include <molerat/base.h>
#include <molerat/queue.h>

void queue_init(struct queue *q, unsigned int max_size)
{
	q->capacity = 8;
	q->items = xalloc(q->capacity * sizeof *q->items);
	q->read = q->write = 0;
	q->max_size = max_size;
	wait_list_init(&q->waiters, 0);
	q->waiters_state = NEITHER;
}

void queue_fini(struct queue *q)
{
	free(q->items);
	wait_list_fini(&q->waiters);
}

static void flip_waiters_state(struct queue *q, enum queue_waiters_state st)
{
	if (q->waiters_state != st) {
		wait_list_set(&q->waiters, 0, TRUE);
		q->waiters_state = st;
	}
}

bool_t queue_push(struct queue *q, void *item, struct tasklet *t)
{
	unsigned int size = q->write - q->read;

	if (size >= q->max_size)
		/* Queue full */
		flip_waiters_state(q, WAS_FULL);

	if (q->waiters_state == WAS_FULL && !wait_list_down(&q->waiters, 1, t))
		return 0;

	if (size == q->capacity) {
		/* items full.  Resize. */
		unsigned int capacity = q->capacity * 2;
		void **items = xalloc(capacity * sizeof *items);

		memcpy(items, q->items + q->read,
		       (q->capacity - q->read) * sizeof *items);
		memcpy(items + q->capacity - q->read, q->items,
		       (q->write - q->capacity) * sizeof *items);
		free(q->items);
		q->items = items;
		q->capacity = capacity;
		q->write -= q->read;
		q->read = 0;
	}

	q->items[q->write++ & (q->capacity - 1)] = item;
	if (q->waiters_state == WAS_EMPTY)
		wait_list_up(&q->waiters, 1);

	return 1;
}

void *queue_shift(struct queue *q, struct tasklet *t)
{
	void *res;

	if (q->read == q->write)
		/* Queue empty */
		flip_waiters_state(q, WAS_EMPTY);

	if (q->waiters_state == WAS_EMPTY && !wait_list_down(&q->waiters, 1, t))
		return NULL;

	assert(q->read != q->write);

	res = q->items[q->read++];
	if (q->read == q->capacity) {
		q->read = 0;
		q->write -= q->capacity;
	}

	if (q->waiters_state == WAS_FULL)
		wait_list_up(&q->waiters, 1);

	return res;

}
