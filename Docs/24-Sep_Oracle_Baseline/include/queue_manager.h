#ifndef QUEUE_MANAGER_H
#define QUEUE_MANAGER_H

/* ======================================================================
 * queue_manager.h
 *
 * The "Dispatcher" component from the proposal's architecture diagram
 * (File Consumer -> Dispatcher -> Workers). Named queue_manager rather
 * than "dispatcher" to avoid colliding with the existing dispatcher.c/
 * .h, which is a different thing entirely (SQL-operation-level dispatch
 * by INSERT/UPDATE/SELECT/etc. - extracted in Stage 1, predates this
 * proposal's own use of the word "Dispatcher" for the queue-routing
 * layer).
 *
 * N fixed-depth queues (dispatcher_queue_count / dispatcher_queue_depth
 * from consumer_file.ini), round-robin assignment on enqueue.
 *
 * Stage 5 update: each queue now has its own mutex + condition
 * variable. File Consumer (the one thread that enqueues) and each
 * queue's dedicated worker thread (the one thread that dequeues from
 * it - see worker.h) form a classic single-producer/single-consumer
 * pattern per queue, so a per-queue lock (rather than one lock for the
 * whole manager) avoids unrelated queues contending with each other.
 * Workers block on their queue's condition variable when it's empty
 * (long-running threads, not exit-when-empty - thread startup is
 * expensive, so these get created once and live for the process's
 * lifetime) rather than the old Stage 4 "drain what's there and
 * return" model. queue_manager_shutdown() wakes every blocked worker
 * cleanly for a graceful stop.
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
 *
 * Only File Consumer's single scanning thread is expected to call
 * queue_manager_enqueue() - the round-robin cursor itself is therefore
 * NOT separately mutex-protected here. If File Consumer itself ever
 * becomes multi-threaded, that cursor needs its own lock too.
 * ====================================================================== */

#include "request_object.h"

typedef struct queue_manager queue_manager_t;   /* opaque */

/*
 * queue_manager_create()
 *
 * queue_count/queue_depth normally come straight from
 * config->dispatcher_queue_count / dispatcher_queue_depth. Returns
 * NULL on allocation failure, mutex/condvar init failure, or if either
 * argument is <= 0.
 */
queue_manager_t *queue_manager_create(int queue_count, int queue_depth);

/*
 * queue_manager_destroy()
 *
 * Frees any RequestObjects still sitting in the queues (via
 * request_object_free()) before freeing the queues themselves - a
 * safety net, not the expected path. Call queue_manager_shutdown()
 * and join all worker threads BEFORE calling this - destroying the
 * mutexes/condvars out from under a still-running worker is undefined
 * behaviour. Safe to call with NULL.
 */
void queue_manager_destroy(queue_manager_t *qm);

/*
 * queue_manager_enqueue()
 *
 * Round-robin enqueue of req (ownership transferred to the queue on
 * success). Signals the target queue's condition variable so a
 * blocked worker wakes up. Returns 0 on success, -1 if every queue is
 * full (in which case the caller still owns req and must free it
 * themselves - the caller is expected to build a QUEUE_FULL error
 * response instead). Implemented as
 * queue_manager_enqueue_excluding(qm, req, -1) - see that function
 * below.
 */
int queue_manager_enqueue(queue_manager_t *qm, request_object_t *req);

/*
 * queue_manager_enqueue_to()   (Contention Manager proposal, 2026-08-08)
 *
 * Enqueues req to a specific queue_index directly, bypassing the
 * round-robin cursor entirely - deliberately no overflow to another
 * queue if the target is full. The whole point of routing to a
 * specific queue (e.g. a dedicated single writer queue for INSERT/
 * UPDATE/DELETE, keeping that traffic on one connection to avoid the
 * cross-worker row-lock contention this project hit repeatedly) is
 * defeated if a full target queue silently spills onto a different
 * one. Returns 0 on success, -1 if the target queue is full (caller
 * still owns req and must free it - same QUEUE_FULL contract as
 * queue_manager_enqueue()).
 */
int queue_manager_enqueue_to(queue_manager_t *qm, request_object_t *req, int queue_index);

/*
 * queue_manager_enqueue_excluding()   (Contention Manager proposal, 2026-08-08)
 *
 * Same round-robin-with-overflow behaviour as queue_manager_enqueue(),
 * but the rotation never considers exclude_index - used to keep
 * normal (SELECT/EXECUTE_PROCEDURE) traffic off a dedicated writer
 * queue reserved via queue_manager_enqueue_to(). Pass -1 for
 * exclude_index to behave identically to queue_manager_enqueue()
 * (which is exactly how that function is implemented).
 */
int queue_manager_enqueue_excluding(queue_manager_t *qm, request_object_t *req, int exclude_index);

/*
 * queue_manager_dequeue_blocking()
 *
 * Called by exactly one dedicated worker thread per queue_index (see
 * worker.h - each worker owns one queue for its whole lifetime).
 * Blocks on that queue's condition variable while it's empty. Returns
 * the next item once one is available, or NULL once
 * queue_manager_shutdown() has been called AND the queue is empty -
 * that NULL is the worker's signal to stop looping and exit. Ownership
 * of the returned request_object_t transfers to the caller - free it
 * with request_object_free() once done.
 */
request_object_t *queue_manager_dequeue_blocking(queue_manager_t *qm, int queue_index);

/*
 * queue_manager_shutdown()
 *
 * Sets the shutdown flag and broadcasts every queue's condition
 * variable so any worker currently blocked in
 * queue_manager_dequeue_blocking() wakes up, sees the flag, finishes
 * draining whatever's left in its queue, then returns NULL on its next
 * call - the signal for that worker's thread to exit cleanly. Does not
 * itself join any threads; the caller still needs to pthread_join()
 * each worker after calling this (see worker_pool_shutdown_and_join()
 * in worker.h, which does both steps together).
 */
void queue_manager_shutdown(queue_manager_t *qm);

/*
 * queue_manager_total_count()
 *
 * Total items currently queued across all queues combined (briefly
 * locks each queue to read its count). Useful for logging/metrics -
 * treat the result as a snapshot, not a guarantee, under concurrent
 * access.
 */
int queue_manager_total_count(queue_manager_t *qm);

#endif /* QUEUE_MANAGER_H */
