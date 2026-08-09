/* ======================================================================
 * generic_queue.c
 *
 * See generic_queue.h for the full design rationale.
 * ====================================================================== */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#include "generic_queue.h"

struct generic_queue {
    void                 **items;       /* circular buffer of owned void* items */
    int                     capacity;
    int                     count;
    int                     head;
    int                     tail;
    pthread_mutex_t         mutex;
    pthread_cond_t          not_empty;
    volatile int            shutting_down;
    generic_queue_free_fn   free_fn;
};

generic_queue_t *generic_queue_create(int depth, generic_queue_free_fn free_fn)
{
    if (depth <= 0) return NULL;

    generic_queue_t *q = malloc(sizeof(generic_queue_t));
    if (!q) return NULL;

    q->items = calloc((size_t)depth, sizeof(void *));
    if (!q->items) { free(q); return NULL; }

    if (pthread_mutex_init(&q->mutex, NULL) != 0)
    {
        free(q->items);
        free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->not_empty, NULL) != 0)
    {
        pthread_mutex_destroy(&q->mutex);
        free(q->items);
        free(q);
        return NULL;
    }

    q->capacity      = depth;
    q->count         = 0;
    q->head          = 0;
    q->tail          = 0;
    q->shutting_down = 0;
    q->free_fn       = free_fn;

    return q;
}

void generic_queue_destroy(generic_queue_t *q)
{
    if (!q) return;

    while (q->count > 0)
    {
        void *item = q->items[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        if (q->free_fn && item) q->free_fn(item);
    }

    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->mutex);
    free(q->items);
    free(q);
}

int generic_queue_enqueue(generic_queue_t *q, void *item)
{
    if (!q || !item) return -1;

    pthread_mutex_lock(&q->mutex);

    if (q->count >= q->capacity)
    {
        /* Full - drop rather than block, same policy as
         * session_touch_queue.h. Caller keeps ownership and decides
         * what to do (typically: log and free it).                    */
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    q->items[q->tail] = item;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);

    return 0;
}

/* Internal: pop one item off the front. Caller must hold q->mutex and
 * have already confirmed q->count > 0.                                */
static void *pop_locked(generic_queue_t *q)
{
    void *item = q->items[q->head];
    q->items[q->head] = NULL;
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return item;
}

void *generic_queue_dequeue_blocking(generic_queue_t *q)
{
    pthread_mutex_lock(&q->mutex);

    while (q->count == 0 && !q->shutting_down)
        pthread_cond_wait(&q->not_empty, &q->mutex);

    void *item = (q->count > 0) ? pop_locked(q) : NULL;

    pthread_mutex_unlock(&q->mutex);
    return item;   /* NULL only when empty AND shutting down */
}

void *generic_queue_dequeue_timed(generic_queue_t *q, int timeout_ms, int *timed_out)
{
    *timed_out = 0;

    if (!q) return NULL;

    if (timeout_ms <= 0)
    {
        /* Zero-wait poll, deliberately not routed through
         * pthread_cond_timedwait() at all - an already-past deadline
         * is a valid input to it on paper, but there's no reason to
         * pay for the clock_gettime()/timedwait() machinery just to
         * express "check right now, don't wait".                      */
        pthread_mutex_lock(&q->mutex);
        void *item = (q->count > 0) ? pop_locked(q) : NULL;
        if (!item) *timed_out = (q->shutting_down ? 0 : 1);
        pthread_mutex_unlock(&q->mutex);
        return item;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&q->mutex);

    int wait_rc = 0;
    while (q->count == 0 && !q->shutting_down && wait_rc == 0)
        wait_rc = pthread_cond_timedwait(&q->not_empty, &q->mutex, &deadline);

    /* wait_rc == ETIMEDOUT is possible even if an item arrived in the
     * same instant the deadline passed - always check q->count itself
     * rather than trusting wait_rc alone, same reasoning as the
     * classic spurious-wakeup guard on any condvar wait.               */
    void *item = (q->count > 0) ? pop_locked(q) : NULL;

    if (!item)
    {
        /* Empty either because we timed out, or because shutdown was
         * signalled - tell these apart for the caller explicitly,
         * exactly as generic_queue.h's own doc comment promises.       */
        *timed_out = q->shutting_down ? 0 : 1;
    }

    pthread_mutex_unlock(&q->mutex);
    return item;
}

void generic_queue_shutdown(generic_queue_t *q)
{
    if (!q) return;

    pthread_mutex_lock(&q->mutex);
    q->shutting_down = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}
