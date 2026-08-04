#ifndef RESPONSE_OBJECT_H
#define RESPONSE_OBJECT_H

/* ======================================================================
 * response_object.h
 *
 * Stage 3 (File_Consumer_proposal v1.2) - the ResponseObject that
 * flows from process_xml_file() (dispatcher.c) to the Response Manager
 * (response_manager.c), which writes it out to Output_* / Error_*.
 *
 * Kept as its own tiny header (rather than living in dispatcher.h or
 * response_manager.h) purely to avoid a circular include between the
 * two - both need the struct, neither should have to include the
 * other's header just to get it.
 *
 * Content availability, honestly stated:
 *   - PASS: response_body is the real XML/JSON result body produced by
 *     execute_query_batch()/execute_insert_batch()/etc. - exactly what
 *     used to be logged and thrown away before Stage 3.
 *   - ERROR from Level 1/Level 2 (parse/validation failures caught in
 *     process_xml_file() itself): error_code/error_text are the real,
 *     specific values from level1_error / operation->validation_status
 *     - good detail.
 *   - ERROR from execution itself (e.g. an OCI error inside
 *     execute_insert_batch()): none of the execute_*_batch() functions
 *     currently return structured error detail on failure - cfg->xml
 *     and cfg->OUTPUT_JSON are only ever populated on their success
 *     path (verified in OCI_Insert_Execute_Module.c - the "Stage 5:
 *     Building result response" block sits after every error path's
 *     own "goto Cleanup"). So these responses get a generic
 *     error_code/error_text pointing at the relevant subsystem log
 *     (insert_Data_Manager.log etc.) for the real detail, rather than
 *     a fabricated specific reason. Getting execute_*_batch() to
 *     return structured error detail directly would remove the need
 *     for that log cross-reference - worth doing later, out of scope
 *     for this stage.
 * ====================================================================== */

typedef enum {
    RESPONSE_STATUS_PASS  = 0,
    RESPONSE_STATUS_ERROR = 1
} response_status_t;

typedef struct {
    response_status_t status;

    char   audit_id[128];     /* from external_audit_id, or "-" if this
                                  request never got far enough to have
                                  one (e.g. Level 1 parse failure)      */
    char   operation[32];     /* "SELECT" | "INSERT" | ... | "-"       */

    char   error_code[64];    /* "-" when status == PASS               */
    char   error_text[512];   /* "-" when status == PASS               */

    char  *response_body;     /* heap-allocated XML or JSON body.
                                  Always non-NULL after
                                  process_xml_file() returns - either a
                                  real result body (PASS) or a
                                  synthesized error envelope (ERROR).
                                  Caller owns it; free via
                                  response_object_free().               */
    int    is_json;           /* 1 = response_body is JSON, 0 = XML    */
} response_object_t;

/* Zero-initialise - audit_id/operation/error_code/error_text all start
 * as "-" so a response_object_t is always safe to write out even if
 * some code path forgets to set a field.                              */
void response_object_init(response_object_t *resp);

/* Frees response_body (if set) and resets the struct via
 * response_object_init(). Does NOT free resp itself - callers own the
 * struct's storage (typically a stack local).                         */
void response_object_free(response_object_t *resp);

#endif /* RESPONSE_OBJECT_H */
