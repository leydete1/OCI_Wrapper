/*
 * OCI_Level1_Parser.h
 *
 * Level 1 Parser
 * ----------------
 * First stage of the format-agnostic input pipeline (see
 * OCI_Request_Response_Types.h and Data_Manager_Request_Definitions.docx
 * for the full design). Responsible for exactly four things, no more:
 *
 *   1. Determine document type (XML or JSON) by sniffing content, not
 *      file extension - this needs to work identically whether the
 *      input is a file on disk today or an HTTP body later, and an
 *      HTTP body has no filename to go by.
 *   2. Load into a DOM (libxml2) or JSON tree (cJSON) and confirm the
 *      document is well-formed. A parse failure here IS the Level 1
 *      failure - stop, do not proceed to step 3.
 *   3. Confirm mandatory envelope fields are present: external_audit_id,
 *      session_id, and at least one operation.
 *
 *      session_id is checked for PRESENCE only (even the literal
 *      placeholder "-" satisfies this) - not for whether "-" is an
 *      acceptable value to actually execute an operation against. That
 *      stronger judgment belongs to real session validation at CRUD
 *      dispatch time, once session enforcement is wired in - it is not
 *      Level 1's job, and every non-CREATE_SESSION test fixture built
 *      so far uses "-" as its standard stub, matching the convention
 *      used throughout the rest of the project's testing to date.
 *
 *   4. On success, return a populated input_c_request_t (envelope
 *      fields + one input_c_operation_t per <operation>, with type set
 *      but payload left NULL - Level 2 owns building the concrete
 *      per-operation payload struct and doing per-operation validation).
 *      On failure at any step, populate error_detail and return
 *      non-zero; do not partially populate the request.
 *
 * Libraries
 * ---------
 *   XML:  libxml2   (sudo apt install libxml2-dev, link -lxml2)
 *   JSON: cJSON      (single cJSON.c/cJSON.h pair, drop into source tree)
 *
 * Logging
 * -------
 * Uses the main logger for now (ctx->logger), per 2026-07-16 decision -
 * this is a spike to prove the parsing layer out. Worth splitting into
 * its own logger later the same way session_logger was split out,
 * once this graduates from spike to real integration.
 */

#ifndef OCI_LEVEL1_PARSER_H
#define OCI_LEVEL1_PARSER_H

#include "OCI_Connection.h"
#include "OCI_Request_Response_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Result codes                                                        */
/* ------------------------------------------------------------------ */
#define LEVEL1_OK                   0
#define LEVEL1_ERR_EMPTY_INPUT     -1   /* zero-length input                */
#define LEVEL1_ERR_UNKNOWN_FORMAT  -2   /* first byte isn't '<', '{', or '[' */
#define LEVEL1_ERR_MALFORMED       -3   /* failed to parse as XML/JSON       */
#define LEVEL1_ERR_MISSING_FIELD   -4   /* a mandatory envelope field absent */
#define LEVEL1_ERR_ALLOC           -5

/*
 * level1_detect_format()
 *
 * Sniffs the first non-whitespace byte of buf: '<' -> XML, '{' or '[' ->
 * JSON, anything else -> INPUT_FORMAT_UNKNOWN. Deliberately does not
 * look at a filename or extension - this must work identically for a
 * file read today and an HTTP body later, and the latter has no
 * filename at all.
 */
input_format_t level1_detect_format(const char *buf, size_t len);

/*
 * level1_parse()
 *
 * Runs all four steps described in the header comment above against
 * one request document (already read into memory - this function does
 * not touch the filesystem or network itself).
 *
 * Parameters
 *   ctx          - for ctx->logger (see Logging note above)
 *   buf, len     - the raw request bytes
 *   out_request  - populated on success only (LEVEL1_OK). Caller owns
 *                  the returned input_c_request_t and its operations
 *                  array - see level1_free_request().
 *   error_detail - populated on any failure (non-zero return). On
 *                  success, error_detail->status_code is set to 0 and
 *                  error_code/error_text are set to "-", matching the
 *                  project-wide operation_status_t convention.
 *
 * Returns LEVEL1_OK (0) on success, one of the LEVEL1_ERR_* codes above
 * otherwise. Never partially populates out_request on failure.
 */
int level1_parse(oci_context_t       *ctx,
                  const char          *buf,
                  size_t               len,
                  input_c_request_t   *out_request,
                  operation_status_t  *error_detail);

/*
 * level1_free_request()
 *
 * Frees the operations array (and, once Level 2 exists, whatever it
 * allocated into each operation's payload) allocated by a successful
 * level1_parse(). Safe to call on a zeroed/never-populated request.
 */
void level1_free_request(input_c_request_t *request);

#ifdef __cplusplus
}
#endif

#endif /* OCI_LEVEL1_PARSER_H */
