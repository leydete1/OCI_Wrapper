/* ======================================================================
 * queue_manager.c
 *
 * See queue_manager.h for the full design rationale.
 * ====================================================================== */

#include <stdlib.h>

#include "queue_manager.h"

typedef struct {
    request_object_t **items;   /* circular buffer, size == capacity */
    int capacity;
    int count;
    int head;                   /* next slot to dequeue from */
    int tail;                   /* next slot to enqueue into */
} single_queue_t;

struct queue_manager {
    single_queue_t *queues;
    int queue_count;
    int enqueue_cursor;   /* round-robin position for enqueue */
    int dequeue_cursor;   /* separate round-robin position for dequeue */
};

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
    qm->dequeue_cursor = 0;

    for (int i = 0; i < queue_count; i++)
    {
        qm->queues[i].items = calloc((size_t)queue_depth, sizeof(request_object_t *));
        if (!qm->queues[i].items)
        {
            for (int j = 0; j < i; j++) free(qm->queues[j].items);
            free(qm->queues);
            free(qm);
            return NULL;
        }
        qm->queues[i].capacity = queue_depth;
        qm->queues[i].count    = 0;
        qm->queues[i].head     = 0;
        qm->queues[i].tail     = 0;
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
     * fair even after a temporary fill.                               */
    int start = qm->enqueue_cursor;
    qm->enqueue_cursor = (qm->enqueue_cursor + 1) % qm->queue_count;

    for (int i = 0; i < qm->queue_count; i++)
    {
        int idx = (start + i) % qm->queue_count;
        if (!single_queue_is_full(&qm->queues[idx]))
            return single_queue_push(&qm->queues[idx], req);
    }

    return -1;   /* every queue full */
}

request_object_t *queue_manager_dequeue_any(queue_manager_t *qm)
{
    int start = qm->dequeue_cursor;

    for (int i = 0; i < qm->queue_count; i++)
    {
        int idx = (start + i) % qm->queue_count;
        if (!single_queue_is_empty(&qm->queues[idx]))
        {
            qm->dequeue_cursor = (idx + 1) % qm->queue_count;
            return single_queue_pop(&qm->queues[idx]);
        }
    }

    return NULL;   /* every queue empty */
}

int queue_manager_total_count(queue_manager_t *qm)
{
    int total = 0;
    for (int i = 0; i < qm->queue_count; i++)
        total += qm->queues[i].count;
    return total;
}
