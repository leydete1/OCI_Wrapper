#ifndef QUEUE_MANAGER_H
#define QUEUE_MANAGER_H

/* ======================================================================
 * queue_manager.h
 *
 * Stage 4 (File_Consumer_proposal v1.2) - the "Dispatcher" component
 * from the proposal's architecture diagram (File Consumer -> Dispatcher
 * -> Workers). Named queue_manager rather than "dispatcher" to avoid
 * colliding with the existing dispatcher.c/.h, which is a different
 * thing entirely (SQL-operation-level dispatch by INSERT/UPDATE/SELECT/
 * etc. - extracted in Stage 1, predates this proposal's own use of the
 * word "Dispatcher" for the queue-routing layer).
 *
 * N fixed-depth queues (dispatcher_queue_count / dispatcher_queue_depth
 * from consumer_file.ini), round-robin assignment. Deliberately no
 * mutex/condvar yet - single-threaded for this stage (File Consumer
 * enqueues everything from one directory scan, then the single worker
 * drains it all afterward, synchronously - see worker.h). Thread
 * safety is Stage 5's job, once there's more than one thread touching
 * these queues at once.
 *
 * Queue-Full behaviour (Queue-Full Behavior addendum, merged into the
 * proposal v1.2): round-robin tries the next queue in rotation; if
 * that one happens to be full but another queue has room, it falls
 * through to the next available queue rather than rejecting outright -
 * only a genuinely full *set* (every queue at depth) triggers
 * QUEUE_FULL. This is a deliberate reading of the addendum's own
 * wording ("when all queues full, reject immediately") - worth
 * flagging in case a stricter "reject the moment the assigned queue is
 * full, no overflow to other queues" interpretation was intended
 * instead; easy to switch if so.
 * ====================================================================== */

#include "request_object.h"

typedef struct queue_manager queue_manager_t;   /* opaque */

/*
 * queue_manager_create()
 *
 * queue_count/queue_depth normally come straight from
 * config->dispatcher_queue_count / dispatcher_queue_depth. Returns
 * NULL on allocation failure or if either argument is <= 0.
 */
queue_manager_t *queue_manager_create(int queue_count, int queue_depth);

/*
 * queue_manager_destroy()
 *
 * Frees any RequestObjects still sitting in the queues (via
 * request_object_free()) before freeing the queues themselves - a
 * safety net for the case something aborts mid-run with items still
 * queued, not the expected path (Stage 4's enqueue-all-then-drain-all
 * flow should always leave the queues empty by the time this runs).
 * Safe to call with NULL.
 */
void queue_manager_destroy(queue_manager_t *qm);

/*
 * queue_manager_enqueue()
 *
 * Round-robin enqueue of req (ownership transferred to the queue on
 * success). Returns 0 on success, -1 if every queue is full (in which
 * case the caller still owns req and must free it themselves - the
 * caller is expected to build a QUEUE_FULL error response instead).
 */
int queue_manager_enqueue(queue_manager_t *qm, request_object_t *req);

/*
 * queue_manager_dequeue_any()
 *
 * Pulls the next item in round-robin order across all queues (its own
 * separate cursor from the enqueue side, so drain order isn't
 * required to match enqueue order queue-by-queue). Returns NULL once
 * every queue is empty. Ownership of the returned request_object_t
 * transfers to the caller - free it with request_object_free() once
 * done.
 */
request_object_t *queue_manager_dequeue_any(queue_manager_t *qm);

/*
 * queue_manager_total_count()
 *
 * Total items currently queued across all queues combined. Useful for
 * logging/metrics.
 */
int queue_manager_total_count(queue_manager_t *qm);

#endif /* QUEUE_MANAGER_H */
