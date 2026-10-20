/*
 * lob_text_bind.h   (driver_mssql Stage 4, 2026-10-20, point U6)
 *
 * Core helper for a text LOB that the driver binds in the INSERT/UPDATE
 * statement itself instead of writing it afterwards - db_type_text_bound()
 * (db_type_class.h) says which columns: SQL Server's varchar(max),
 * nvarchar(max), text, ntext and xml; never an Oracle type.
 *
 * The value follows the same rule as the CLOB written after the
 * statement: a value starting "file://" is replaced by that file's
 * contents; anything else is the text itself.
 *
 *   raw        the whole client value (never cut at 32 KB)
 *   out_value  what to bind - raw itself, or *out_heap
 *   out_heap   the file contents (caller frees), or NULL
 *   out_len    byte length of *out_value
 *
 * Returns 0, or -1 (logged on logger) when the file cannot be read.
 */

#ifndef LOB_TEXT_BIND_H
#define LOB_TEXT_BIND_H

#include <stddef.h>
#include "logger.h"

#ifdef __cplusplus
extern "C" {
#endif

int lob_text_resolve(const char *raw, const char **out_value, char **out_heap,
                     size_t *out_len, logger_t *logger);

#ifdef __cplusplus
}
#endif

#endif /* LOB_TEXT_BIND_H */
