#ifndef REQUEST_OBJECT_H
#define REQUEST_OBJECT_H

/* ======================================================================
 * request_object.h
 *
 * Stage 4 (File_Consumer_proposal v1.2) - the RequestObject that flows
 * from File Consumer -> queue_manager -> worker. Exists specifically
 * to satisfy the Payload Ownership addendum: File Consumer alone reads
 * the file and populates payload/payload_length here; everything
 * downstream (queue, worker, dispatcher) works only with the payload,
 * never the filesystem directly. input_path/processing_path are
 * carried along purely as traceability metadata, per that same
 * addendum - not touched by anything except the Response Manager at
 * the very end (to move the original file to Output_* / Error_*).
 * ====================================================================== */

typedef struct {
    char  *payload;            /* heap-allocated file contents, NUL-terminated.
                                   Owned by this struct - freed by
                                   request_object_free(). */
    long   payload_length;

    char   filename[256];          /* bare filename, no directory */
    char   processing_path[1024];  /* full path where the file currently sits -
                                       Processing_* in the normal case. Traceability
                                       metadata only, per Payload Ownership - the
                                       Response Manager is the only thing that acts
                                       on this (moving the file to its final home). */
    char   output_dir[256];        /* Output_XML or Output_JSON, matching format */
    char   error_dir[256];         /* Error_XML or Error_JSON, matching format */

    char   session_id[37];         /* SESSION_UUID_LEN (session_cache.h) - the
                                       real session_id File Consumer is holding
                                       for this run (Session Manager proposal,
                                       Stage 1, 2026-08-06). Stamped onto the
                                       parsed request in dispatcher.c, overriding
                                       whatever the raw payload itself carried
                                       (almost always "-" today) - this becomes
                                       the value Stage 3's future validation
                                       actually checks, not just a log label. */
} request_object_t;

/*
 * request_object_create()
 *
 * Allocates a request_object_t and takes ownership of payload (the
 * caller must not free it after a successful call - it's now owned by
 * the returned object). Returns NULL on allocation failure, in which
 * case the caller still owns payload and must free it themselves.
 *
 * session_id is File Consumer's own real session for this run (Session
 * Manager proposal, Stage 1) - pass NULL or "" if genuinely unavailable
 * (e.g. session creation itself failed) and dispatcher.c will fall
 * back to whatever the payload's own session_id says, same as before
 * this stage existed.
 */
request_object_t *request_object_create(char       *payload,
                                          long        payload_length,
                                          const char *filename,
                                          const char *processing_path,
                                          const char *output_dir,
                                          const char *error_dir,
                                          const char *session_id);

/*
 * request_object_free()
 *
 * Frees req->payload and req itself. Safe to call with NULL.
 */
void request_object_free(request_object_t *req);

#endif /* REQUEST_OBJECT_H */
