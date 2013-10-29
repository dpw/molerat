#ifndef MOLERAT_QUEUE_H
#define MOLERAT_QUEUE_H

#include "base.h"
#include "tasklet.h"

enum queue_waiters_state { NEITHER, WAS_FULL, WAS_EMPTY };

struct queue {
        void **items;

	/* Size of items.  Always a power of two. */
        unsigned int capacity;

	/* Read index.  read < capacity and read <= write */
        unsigned int read;

	/* Write index (modulo capacity).  write < capacity * 2 */
        unsigned int write;

	/* Maximum size of the queue. */
	unsigned int max_size;

	/* This is used both when waiting for the queue to become
	 * non-empty, and waiting for the queue to become non-full. */
	struct wait_list waiters;
	enum queue_waiters_state waiters_state;
};

void queue_init(struct queue *q, unsigned int max_size);
void queue_fini(struct queue *q);

bool_t queue_push(struct queue *q, void *item, struct tasklet *t);
void *queue_shift(struct queue *q, struct tasklet *t);

#endif
