/* ======================================================================
 * queue_manager.c
 *
 * See queue_manager.h for the full design rationale.
 * ====================================================================== */

#include <stdlib.h>
#include <pthread.h>

#include "queue_manager.h"

typedef struct {
    request_object_t **items;   /* circular buffer, size == capacity */
    int capacity;
    int count;
    int head;                   /* next slot to dequeue from */
    int tail;                   /* next slot to enqueue into */
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
} single_queue_t;

struct queue_manager {
    single_queue_t *queues;
    int queue_count;
    int enqueue_cursor;   /* round-robin position for enqueue - see
                              queue_manager.h's own note on why this
                              isn't separately lock-protected */
    volatile int shutting_down;
};

/* ------------------------------------------------------------------ */
/*  Internal helpers - caller must already hold q->mutex.               */
/* ------------------------------------------------------------------ */
static int single_queue_is_full(single_queue_t *q)  { return q->count >= q->capacity; }
static int single_queue_is_empty(single_queue_t *q)  { return q->count == 0; }

static int single_queue_push(single_queue_t *q, request_object_t *req)
{
    if (single_queue_is_full(q)) return -1;
    q->items[q->tail] = req;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    return 0;
}

static request_object_t *single_queue_pop(single_queue_t *q)
{
    if (single_queue_is_empty(q)) return NULL;
    request_object_t *req = q->items[q->head];
    q->items[q->head] = NULL;
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return req;
}

/* ------------------------------------------------------------------ */
queue_manager_t *queue_manager_create(int queue_count, int queue_depth)
{
    if (queue_count <= 0 || queue_depth <= 0) return NULL;

    queue_manager_t *qm = malloc(sizeof(queue_manager_t));
    if (!qm) return NULL;

    qm->queues = calloc((size_t)queue_count, sizeof(single_queue_t));
    if (!qm->queues) { free(qm); return NULL; }

    qm->queue_count    = queue_count;
    qm->enqueue_cursor = 0;
    qm->shutting_down  = 0;

    int initialised = 0;   /* how many queues got fully set up, for
                               clean unwind if something fails partway */

    for (int i = 0; i < queue_count; i++)
    {
        qm->queues[i].items = calloc((size_t)queue_depth, sizeof(request_object_t *));
        if (!qm->queues[i].items) break;

        if (pthread_mutex_init(&qm->queues[i].mutex, NULL) != 0)
        {
            free(qm->queues[i].items);
            break;
        }
        if (pthread_cond_init(&qm->queues[i].not_empty, NULL) != 0)
        {
            pthread_mutex_destroy(&qm->queues[i].mutex);
            free(qm->queues[i].items);
            break;
        }

        qm->queues[i].capacity = queue_depth;
        qm->queues[i].count    = 0;
        qm->queues[i].head     = 0;
        qm->queues[i].tail     = 0;
        initialised++;
    }

    if (initialised != queue_count)
    {
        /* Partial init failure - unwind what did succeed and bail.    */
        for (int i = 0; i < initialised; i++)
        {
            pthread_cond_destroy(&qm->queues[i].not_empty);
            pthread_mutex_destroy(&qm->queues[i].mutex);
            free(qm->queues[i].items);
        }
        free(qm->queues);
        free(qm);
        return NULL;
    }

    return qm;
}

void queue_manager_destroy(queue_manager_t *qm)
{
    if (!qm) return;

    for (int i = 0; i < qm->queue_count; i++)
    {
        request_object_t *req;
        while ((req = single_queue_pop(&qm->queues[i])) != NULL)
            request_object_free(req);
        pthread_cond_destroy(&qm->queues[i].not_empty);
        pthread_mutex_destroy(&qm->queues[i].mutex);
        free(qm->queues[i].items);
    }
    free(qm->queues);
    free(qm);
}

int queue_manager_enqueue(queue_manager_t *qm, request_object_t *req)
{
    /* Try the round-robin-assigned queue first, then scan forward
     * (wrapping) for the first queue with room - see queue_manager.h's
     * own doc comment on why this isn't a strict "reject the moment
     * the assigned queue is full" policy. The cursor always advances
     * by one regardless of outcome, so long-run distribution stays
     * fair even after a temporary fill.
     *
     * Only ever locks one queue's mutex at a time - never holds two
     * locks simultaneously while scanning, so there's no lock-ordering
     * deadlock risk here.                                              */
    int start = qm->enqueue_cursor;
    qm->enqueue_cursor = (qm->enqueue_cursor + 1) % qm->queue_count;

    for (int i = 0; i < qm->queue_count; i++)
    {
        int idx = (start + i) % qm->queue_count;
        single_queue_t *q = &qm->queues[idx];

        pthread_mutex_lock(&q->mutex);
        if (!single_queue_is_full(q))
        {
            int rc = single_queue_push(q, req);
            pthread_cond_signal(&q->not_empty);
            pthread_mutex_unlock(&q->mutex);
            return rc;
        }
        pthread_mutex_unlock(&q->mutex);
    }

    return -1;   /* every queue full */
}

request_object_t *queue_manager_dequeue_blocking(queue_manager_t *qm, int queue_index)
{
    single_queue_t *q = &qm->queues[queue_index];

    pthread_mutex_lock(&q->mutex);

    while (single_queue_is_empty(q) && !qm->shutting_down)
        pthread_cond_wait(&q->not_empty, &q->mutex);

    request_object_t *req = single_queue_pop(q);   /* NULL if empty
                                                        (only possible
                                                        here on shutdown) */
    pthread_mutex_unlock(&q->mutex);
    return req;
}

void queue_manager_shutdown(queue_manager_t *qm)
{
    if (!qm) return;

    for (int i = 0; i < qm->queue_count; i++)
    {
        single_queue_t *q = &qm->queues[i];
        /* Set the flag while holding this queue's own mutex, then
         * broadcast on the same mutex - the textbook-correct way to
         * avoid a missed-wakeup race with condition variables. Setting
         * it redundantly once per queue is harmless (idempotent).      */
        pthread_mutex_lock(&q->mutex);
        qm->shutting_down = 1;
        pthread_cond_broadcast(&q->not_empty);
        pthread_mutex_unlock(&q->mutex);
    }
}

int queue_manager_total_count(queue_manager_t *qm)
{
    int total = 0;
    for (int i = 0; i < qm->queue_count; i++)
    {
        pthread_mutex_lock(&qm->queues[i].mutex);
        total += qm->queues[i].count;
        pthread_mutex_unlock(&qm->queues[i].mutex);
    }
    return total;
}
