#ifndef SESSION_TOUCH_QUEUE_H
#define SESSION_TOUCH_QUEUE_H

/* ======================================================================
 * session_touch_queue.h
 *
 * Session Manager proposal, Stage 2 (2026-08-06). A single bounded
 * queue of session_id strings, feeding the Session Manager's one
 * dedicated thread (session_manager_runner.h). Deliberately simpler
 * than queue_manager.h's N-queue round-robin design - there's only
 * one consumer here, so no need for the multi-queue machinery; this
 * is a plain mutex+condvar bounded FIFO, same locking approach,
 * smaller surface.
 *
 * Confirmed design (2026-08-06): enqueue is fire-and-forget and
 * genuinely non-blocking - a worker calls this right after building a
 * ResponseObject and does not wait on it. If the queue is full, the
 * touch is dropped (logged, not fatal) rather than blocking the
 * calling worker thread - a missed activity-refresh cycle just means
 * last_activity is slightly stale until the next successful touch,
 * not a lost request. This is deliberately lower-stakes than
 * queue_manager.h's own QUEUE_FULL handling, which rejects an entire
 * request with a real error response - a dropped touch has no
 * equivalent user-visible consequence.
 * ====================================================================== */

typedef struct session_touch_queue session_touch_queue_t;   /* opaque */

/*
 * session_touch_queue_create()
 *
 * depth is a fixed capacity - there's no config key for this yet
 * (Stage 2 keeps it simple; see session_manager_runner.c for the
 * value in use). Returns NULL on allocation or mutex/condvar init
 * failure, or if depth <= 0.
 */
session_touch_queue_t *session_touch_queue_create(int depth);

/*
 * session_touch_queue_destroy()
 *
 * Frees any session_id strings still queued (shouldn't happen in the
 * normal shutdown path - see session_manager_runner.h's own drain-on-
 * shutdown behaviour - this is a safety net, not the expected path).
 * Safe to call with NULL.
 */
void session_touch_queue_destroy(session_touch_queue_t *q);

/*
 * session_touch_queue_enqueue()
 *
 * Copies session_id (caller keeps ownership of its own buffer - this
 * function makes its own heap copy). Non-blocking: returns 0 on
 * success, -1 immediately if the queue is currently full (does NOT
 * wait for room) or if session_id is NULL/empty. A -1 here is safe to
 * ignore, per this header's own doc comment above on why a dropped
 * touch isn't a critical failure - the caller (worker.c) just logs it
 * and moves on.
 */
int session_touch_queue_enqueue(session_touch_queue_t *q, const char *session_id);

/*
 * session_touch_queue_dequeue_blocking()
 *
 * Blocks while the queue is empty. Returns a heap-allocated session_id
 * string the caller must free(), or NULL once
 * session_touch_queue_shutdown() has been called AND the queue is
 * empty - the signal for the Session Manager thread to exit.
 */
char *session_touch_queue_dequeue_blocking(session_touch_queue_t *q);

/*
 * session_touch_queue_shutdown()
 *
 * Same shape as queue_manager_shutdown() - sets the shutdown flag and
 * wakes the blocked consumer thread so it can drain whatever's left
 * and exit cleanly. Does not itself join anything - see
 * session_manager_runner.h's own stop_and_join(), which does both
 * steps together.
 */
void session_touch_queue_shutdown(session_touch_queue_t *q);

#endif /* SESSION_TOUCH_QUEUE_H */
