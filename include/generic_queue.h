#ifndef GENERIC_QUEUE_H
#define GENERIC_QUEUE_H

#include <stddef.h>

/* ======================================================================
 * generic_queue.h
 *
 * Metrics refactor (closure item 5), Stage 1 (2026-08-09). A reusable
 * bounded queue with a timeout-capable blocking dequeue - the piece
 * neither of this project's two existing queues (queue_manager.h's
 * N-queue round-robin dispatch queues, session_touch_queue.h's single
 * fire-and-forget touch queue) needed before now. The Metrics DB
 * writer needs "block until an item arrives, or metrics_max_insert_
 * delay expires, whichever first" - a genuinely different shape from
 * "block until an item arrives" alone.
 *
 * This is also the third time this project has built "bounded queue +
 * dedicated consumer thread with its own pooled session" (worker.c,
 * session_manager_runner.c, and now the Metrics Writer) - worth
 * extracting the shared queue mechanics into one reusable module this
 * time, the same reasoning ctx_utils.c's copy_shared_ctx_fields() was
 * extracted on its third use rather than duplicated a fourth time.
 *
 * DELIBERATELY GENERIC (void* items), unlike session_touch_queue.h's
 * own char* session_id design - this queue needs to carry
 * metrics_record_t pointers, not strings. That changes the ownership
 * model from session_touch_queue's "copy the string, caller keeps
 * their own buffer": there is no way to generically "copy" an
 * arbitrary void* without knowing its size/type, so this queue never
 * copies anything.
 *
 * OWNERSHIP: the caller must heap-allocate each item before enqueueing
 * it. On a successful enqueue, ownership transfers to the queue; on a
 * failed enqueue (queue full), the caller keeps ownership and decides
 * what to do (typically: log and free it, matching the "drop rather
 * than block" philosophy already established for the touch queue).
 * Whoever successfully dequeues an item owns it and must free it.
 * generic_queue_destroy() takes an optional free function specifically
 * so any items still queued at shutdown get properly freed regardless
 * of what type they actually are - the queue itself has no idea.
 * ====================================================================== */

typedef struct generic_queue generic_queue_t;   /* opaque */

/* Called by generic_queue_destroy() on any item still queued at
 * destroy time - pass NULL if items never need freeing (e.g. they're
 * always fully drained before destroy() is called), otherwise pass a
 * function matching your item type's own free (e.g. a wrapper around
 * metrics_record_free() cast appropriately, or plain free() if the
 * item is a flat, no-nested-allocations struct).                     */
typedef void (*generic_queue_free_fn)(void *item);

/*
 * generic_queue_create()
 *
 * depth is a fixed capacity. free_fn may be NULL (see above). Returns
 * NULL on allocation or mutex/condvar init failure, or if depth <= 0.
 */
generic_queue_t *generic_queue_create(int depth, generic_queue_free_fn free_fn);

/*
 * generic_queue_destroy()
 *
 * Frees any items still queued via the free_fn passed to create()
 * (skipped per-item if free_fn was NULL - those items are simply
 * leaked, on the assumption the caller already knew the queue would
 * be empty at this point). Safe to call with NULL.
 */
void generic_queue_destroy(generic_queue_t *q);

/*
 * generic_queue_enqueue()
 *
 * Non-blocking: returns 0 on success (ownership of item transfers to
 * the queue), -1 immediately if the queue is currently full (item
 * ownership stays with the caller - does NOT wait for room) or if q
 * or item is NULL.
 */
int generic_queue_enqueue(generic_queue_t *q, void *item);

/*
 * generic_queue_dequeue_blocking()
 *
 * Blocks indefinitely while the queue is empty. Returns the next item
 * (ownership transfers to the caller - free it when done) once one is
 * available, or NULL once generic_queue_shutdown() has been called AND
 * the queue is empty - that NULL is the signal to stop looping and
 * exit. Same contract as session_touch_queue_dequeue_blocking() and
 * queue_manager_dequeue_blocking().
 */
void *generic_queue_dequeue_blocking(generic_queue_t *q);

/*
 * generic_queue_dequeue_timed()
 *
 * Like generic_queue_dequeue_blocking(), but gives up after
 * timeout_ms if nothing becomes available. Three distinct outcomes,
 * told apart via the returned pointer and *timed_out (which must not
 * be NULL):
 *
 *   - Non-NULL return: a real item, ownership transfers to the
 *     caller. *timed_out is set to 0.
 *   - NULL return, *timed_out = 1: timeout_ms elapsed with nothing
 *     available - the queue is still running, just empty. Caller
 *     should typically flush whatever partial batch it's holding and
 *     go back to waiting, per metrics_max_insert_delay's own purpose.
 *   - NULL return, *timed_out = 0: generic_queue_shutdown() was
 *     called and the queue is now empty - same "stop looping and
 *     exit" signal as generic_queue_dequeue_blocking()'s own NULL.
 *
 * timeout_ms <= 0 is treated as "return immediately if empty" (a
 * zero-wait poll) rather than blocking indefinitely - use
 * generic_queue_dequeue_blocking() itself for genuinely unbounded
 * waits, to keep that intent explicit at the call site.
 */
void *generic_queue_dequeue_timed(generic_queue_t *q, int timeout_ms, int *timed_out);

/*
 * generic_queue_shutdown()
 *
 * Same shape as session_touch_queue_shutdown()/queue_manager_shutdown():
 * sets the shutdown flag and wakes every blocked consumer so each can
 * drain whatever's left and exit cleanly. Does not itself join
 * anything or free the queue.
 */
void generic_queue_shutdown(generic_queue_t *q);

#endif /* GENERIC_QUEUE_H */
