/* ======================================================================
 * session_touch_queue.c
 *
 * See session_touch_queue.h for the full design rationale.
 * ====================================================================== */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "session_touch_queue.h"

struct session_touch_queue {
    char   **items;      /* circular buffer of heap-allocated session_id strings */
    int      capacity;
    int      count;
    int      head;
    int      tail;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    volatile int shutting_down;
};

session_touch_queue_t *session_touch_queue_create(int depth)
{
    if (depth <= 0) return NULL;

    session_touch_queue_t *q = malloc(sizeof(session_touch_queue_t));
    if (!q) return NULL;

    q->items = calloc((size_t)depth, sizeof(char *));
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

    return q;
}

void session_touch_queue_destroy(session_touch_queue_t *q)
{
    if (!q) return;

    while (q->count > 0)
    {
        free(q->items[q->head]);
        q->head = (q->head + 1) % q->capacity;
        q->count--;
    }

    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->mutex);
    free(q->items);
    free(q);
}

int session_touch_queue_enqueue(session_touch_queue_t *q, const char *session_id)
{
    if (!q || !session_id || !session_id[0]) return -1;

    char *copy = strdup(session_id);
    if (!copy) return -1;

    pthread_mutex_lock(&q->mutex);

    if (q->count >= q->capacity)
    {
        /* Full - drop rather than block, per this module's own design
         * (see session_touch_queue.h). Caller logs this, not fatal.   */
        pthread_mutex_unlock(&q->mutex);
        free(copy);
        return -1;
    }

    q->items[q->tail] = copy;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);

    return 0;
}

char *session_touch_queue_dequeue_blocking(session_touch_queue_t *q)
{
    pthread_mutex_lock(&q->mutex);

    while (q->count == 0 && !q->shutting_down)
        pthread_cond_wait(&q->not_empty, &q->mutex);

    char *item = NULL;
    if (q->count > 0)
    {
        item = q->items[q->head];
        q->items[q->head] = NULL;
        q->head = (q->head + 1) % q->capacity;
        q->count--;
    }

    pthread_mutex_unlock(&q->mutex);
    return item;   /* NULL only when empty AND shutting down */
}

void session_touch_queue_shutdown(session_touch_queue_t *q)
{
    if (!q) return;

    pthread_mutex_lock(&q->mutex);
    q->shutting_down = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}
