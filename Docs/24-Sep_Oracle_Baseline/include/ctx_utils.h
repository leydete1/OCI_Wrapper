#ifndef CTX_UTILS_H
#define CTX_UTILS_H

/* ======================================================================
 * ctx_utils.h
 *
 * Small shared utility, factored out 2026-08-06 while fixing the bug
 * where file_consumer_runner.c's thread had no DB session of its own
 * (see copy_shared_ctx_fields()'s own comment below for why this
 * exists at all). Previously this exact field list was duplicated in
 * worker.c (its own static copy_shared_ctx_fields()) with a comment
 * explicitly flagging the duplication risk against main()'s own inline
 * copy block for its single worker_ctx - now there's a third caller
 * (file_consumer_runner.c) needing the identical list, which is the
 * point past which "just duplicate it again" stops being reasonable.
 * ====================================================================== */

#include "OCI_Connection.h"   /* oci_context_t */

/*
 * copy_shared_ctx_fields()
 *
 * OCI_Pool_get_session() only populates the OCI connection handles
 * (envhp/errhp/svchp etc.) and pool bookkeeping - every logger
 * pointer, the ini pointer, and the caches remain NULL after that
 * call and must be copied in explicitly from a context that already
 * has them (typically the master ctx set up once in main()).
 *
 * Used by every long-running thread that borrows its own pooled
 * session (worker.c, file_consumer_runner.c) right after
 * OCI_Pool_get_session() succeeds.
 *
 * main()'s own single worker_ctx (used by the legacy fixture-directory
 * test harness in pool mode) still has its own separate inline copy of
 * this same field list in Test_XML_Runner.c, not yet unified with this
 * function - a known, pre-existing duplication risk, not a new one.
 * If a new logger or cache is ever added to oci_context_t, it needs to
 * be added HERE and to that inline block in Test_XML_Runner.c.
 */
void copy_shared_ctx_fields(oci_context_t *dst, oci_context_t *src);

#endif /* CTX_UTILS_H */
