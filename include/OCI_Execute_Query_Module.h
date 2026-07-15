
#ifndef OCI_EXECUTE_QUERY_MODULE_H
#define OCI_EXECUTE_QUERY_MODULE_H

#include "logger.h"
#include "ini_reader.h"
#include "OCI_Connection.h"
#include <oci.h>



int execute_query(oci_context_t *ctx,
                  execute_config_t *cfg);

int get_row_count(oci_context_t *ctx,
                  execute_config_t *cfg,
                  unsigned long long *row_count);
                  
                  
void trim_sql_inplace(char *str, oci_context_t *ctx);

int lookup_blob_index(char (*col_names)[256], int col_count, const char *col_name, oci_context_t *ctx);

int write_blob_to_file( lob_item_t *item, const char *output_dir, oci_context_t *ctx);

void generate_timestamp(char *buffer, size_t size, oci_context_t *ctx);

void build_filename_with_timestamp(const char *original,  char *output,  size_t out_size, int idx, oci_context_t *ctx);                             

void populate_blob_filename(
    lob_item_t *item,
    const char **col_names,
    ub4 col_count,
    char **buffers,
    sb2 *indicators,
    int blob_idx,
    oci_context_t *ctx
);  

int lookup_blob_index_1(const char **col_names, int col_count, const char *col_name, oci_context_t *ctx);

typedef struct xml_builder_t xml_builder_t;
int execute_query_batch(oci_context_t *ctx, execute_config_t *cfg);



  
#endif

