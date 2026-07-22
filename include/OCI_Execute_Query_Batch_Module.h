/*
 * OCI_Execute_Query_Batch_Module.h
 *
 * Public entry point for the array-fetch batch SELECT module.
 * See OCI_Execute_Query_Batch_Module.c for the full design description.
 *
 * This header did not previously exist - execute_query_batch()'s
 * prototype lived in OCI_Execute_Query_Module.h even though it's
 * implemented here. Now that OCI_Execute_Query_Module.c has been
 * removed (execute_query(), get_row_count(), and lookup_blob_index_1()
 * were dead code with no callers), the prototype needed a real home
 * matching every other module's file.c/file.h convention in this
 * project.
 */

#ifndef OCI_EXECUTE_QUERY_BATCH_MODULE_H
#define OCI_EXECUTE_QUERY_BATCH_MODULE_H

#include "OCI_Connection.h"   /* oci_context_t, execute_config_t */

#ifdef __cplusplus
extern "C" {
#endif

int execute_query_batch(oci_context_t *ctx, execute_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* OCI_EXECUTE_QUERY_BATCH_MODULE_H */
