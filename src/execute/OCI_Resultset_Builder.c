/*
 * OCI_Resultset_Builder.c
 *
 * See OCI_Resultset_Builder.h for the full API description.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include "OCI_Resultset_Builder.h"

resultset_t *resultset_create(int record_count, int fields_per_row)
{
    if (record_count < 0 || fields_per_row < 0) return NULL;

    resultset_t *rs = calloc(1, sizeof(resultset_t));
    if (!rs) return NULL;

    rs->record_count = record_count;
    rs->records = calloc((size_t)record_count, sizeof(resultset_row_t));
    if (!rs->records)
    {
        free(rs);
        return NULL;
    }

    for (int i = 0; i < record_count; i++)
    {
        rs->records[i].record_number = i + 1;   /* 1-based, matches abs_rownum */
        rs->records[i].field_count   = fields_per_row;
        rs->records[i].fields = calloc((size_t)fields_per_row, sizeof(resultset_field_t));

        if (!rs->records[i].fields)
        {
            /* Unwind everything allocated so far - no partial resultset_t
             * is ever handed back to the caller.                        */
            for (int j = 0; j < i; j++)
                free(rs->records[j].fields);
            free(rs->records);
            free(rs);
            return NULL;
        }
    }

    return rs;
}

resultset_row_t *resultset_get_row(resultset_t *rs, int record_number)
{
    if (!rs || record_number < 1 || record_number > rs->record_count)
        return NULL;
    return &rs->records[record_number - 1];
}

int resultset_set_field(resultset_row_t *row,
                         int              field_index,
                         const char      *field_name,
                         const char      *field_type,
                         const char      *value)
{
    if (!row || field_index < 0 || field_index >= row->field_count)
        return -1;

    resultset_field_t *f = &row->fields[field_index];

    if (field_name)
        strncpy(f->field_name, field_name, sizeof(f->field_name) - 1);
    if (field_type)
        strncpy(f->field_type, field_type, sizeof(f->field_type) - 1);
    if (value)
        strncpy(f->value, value, sizeof(f->value) - 1);

    f->is_blob = 0;

    return 0;
}

int resultset_set_blob_field(resultset_row_t *row,
                              int              field_index,
                              const char      *field_name,
                              const char      *file_name,
                              const char      *file_path,
                              const char      *file_url,
                              uint64_t         file_size,
                              const char      *mime_type)
{
    if (!row || field_index < 0 || field_index >= row->field_count)
        return -1;

    resultset_field_t *f = &row->fields[field_index];

    if (field_name)
        strncpy(f->field_name, field_name, sizeof(f->field_name) - 1);
    strncpy(f->field_type, "BLOB", sizeof(f->field_type) - 1);

    f->is_blob = 1;
    f->value[0] = '\0';   /* meaningless for BLOB - keep it empty, not stale */

    resultset_blob_detail_t *b = &f->blob_detail;
    memset(b, 0, sizeof(*b));

    if (file_name)
        strncpy(b->file_name, file_name, sizeof(b->file_name) - 1);
    if (file_path)
        strncpy(b->file_path, file_path, sizeof(b->file_path) - 1);
    if (file_url)
        strncpy(b->file_url, file_url, sizeof(b->file_url) - 1);
    if (mime_type)
        strncpy(b->mime_type, mime_type, sizeof(b->mime_type) - 1);
    b->file_size = file_size;

    return 0;
}

void resultset_free(resultset_t *rs)
{
    if (!rs) return;

    for (int i = 0; i < rs->record_count; i++)
        free(rs->records[i].fields);

    free(rs->records);
    free(rs);
}
