#include "ctx_utils.h"

void copy_shared_ctx_fields(oci_context_t *dst, oci_context_t *src)
{
    dst->logger                = src->logger;
    dst->select_logger         = src->select_logger;
    dst->cache_logger          = src->cache_logger;
    dst->Metadata_logger       = src->Metadata_logger;
    dst->connection_logger     = src->connection_logger;
    dst->connectionpool_logger = src->connectionpool_logger;
    dst->insert_logger         = src->insert_logger;
    dst->update_logger         = src->update_logger;
    dst->delete_logger         = src->delete_logger;
    dst->dml_logger            = src->dml_logger;
    dst->ddl_logger            = src->ddl_logger;
    dst->procedure_logger      = src->procedure_logger;
    dst->ini                   = src->ini;
    dst->resultset_cache       = src->resultset_cache;
    dst->error_logger          = src->error_logger;
    dst->metrics_logger        = src->metrics_logger;
    dst->metrics_writer        = src->metrics_writer;   /* closure item 5, Stage 2 */
    dst->metrics_writer_logger = src->metrics_writer_logger;   /* Stage 2 follow-up */
    dst->transaction_logger    = src->transaction_logger;
    dst->security_logger       = src->security_logger;
    dst->crypt_logger          = src->crypt_logger;
    dst->audit_logger          = src->audit_logger;
    dst->session_logger        = src->session_logger;
    dst->sql_parser_logger     = src->sql_parser_logger;
    dst->file_consumer_logger  = src->file_consumer_logger;
    dst->dispatcher_logger     = src->dispatcher_logger;
    dst->worker_logger         = src->worker_logger;
    dst->metadata_cache        = src->metadata_cache;
    dst->session_cache         = src->session_cache;
    dst->authz_cache           = src->authz_cache;   /* Security Module
                                                       * Stage 5, 2026-08-31 -
                                                       * missing here was the
                                                       * actual cause of every
                                                       * authz_cache_store()
                                                       * call silently failing
                                                       * on worker threads:
                                                       * ctx->authz_cache was
                                                       * correctly set on the
                                                       * bootstrap ctx, but
                                                       * every per-worker ctx
                                                       * copy left it NULL,
                                                       * since this field
                                                       * didn't exist yet when
                                                       * this function was
                                                       * last written. */
}
