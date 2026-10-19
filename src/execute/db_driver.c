/*
 * db_driver.c
 *
 * db_driver_get() - see db_driver.h for the contract.
 *
 * Not a real dispatch yet: db_type does not exist in config.ini /
 * app_config_t yet (order-of-attack step 3, not done). Oracle is also
 * the only driver that exists (order-of-attack step 2, this pass).
 * Both of those are true simultaneously right now, so this always
 * returns oracle_driver_get() regardless of ctx. ctx is accepted (not
 * ignored at the signature level) purely so every call site is already
 * written the way it will need to look once this becomes a real
 * ctx->ini->db_type dispatch - no caller will need to change again when
 * that lands.
 *
 * db_select_int() - item 2b (2026-10-03), a core helper over the
 * cursor interface; see db_driver.h.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db_driver.h"
#include "driver_oracle.h"
#include <strings.h>
#include "logger.h"             /* db_select_int() logging      */
#include "Resultset_Builder.h"  /* resultset_free()             */

/*
 * driver_mssql Stage 2 (2026-10-18), P1 - the real dispatch.
 *
 * ctx->ini->db_type (lower-cased and checked by load_ini(): "oracle"
 * or "mssql") picks the driver. One db_type is active per process, so
 * the last value seen is remembered for the rare caller whose ctx has
 * no ini attached.
 *
 * mssql_driver_get() is a weak reference: a program that does not link
 * driver_mssql.c (the Oracle harnesses) still builds unchanged, and
 * asking it for SQL Server returns NULL with a message - every caller
 * already treats a NULL driver as an error.
 */
extern const db_driver_t *mssql_driver_get(void) __attribute__((weak));

static int g_last_db_is_mssql = 0;

const db_driver_t *db_driver_get(const oci_context_t *ctx)
{
    int is_mssql;

    if (ctx && ctx->ini && ctx->ini->db_type[0])
    {
        is_mssql = strcasecmp(ctx->ini->db_type, "mssql") == 0;
        __atomic_store_n(&g_last_db_is_mssql, is_mssql, __ATOMIC_RELAXED);
    }
    else
        is_mssql = __atomic_load_n(&g_last_db_is_mssql, __ATOMIC_RELAXED);

    if (!is_mssql)
        return oracle_driver_get();

    if (!mssql_driver_get)
    {
        fprintf(stderr, "db_driver_get: db_type=mssql but driver_mssql.c "
                        "is not linked into this program\n");
        return NULL;
    }
    return mssql_driver_get();
}

/*
 * db_select_int() - see db_driver.h. Item 2b (2026-10-03).
 *
 * A correct query returns one row, so after reading it this asks the
 * cursor once more and requires it to be exhausted: a query that
 * returns several rows is rejected rather than silently reading the
 * first one. (The request asks for a batch of 2 so that check usually
 * costs nothing - both rows arrive in the first fetch - but a driver
 * may cut the batch size, so the second fetch is still made whenever
 * the first returned exactly one row.)
 */
int db_select_int(oci_context_t *ctx, const char *sql, int query_timeout,
                  long long *out_value)
{
    if (!ctx || !sql || !out_value)
        return -1;

    const db_driver_t *driver = db_driver_get(ctx);
    if (!driver || !driver->select_open || !driver->select_fetch_batch ||
        !driver->select_close)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "db_driver_get() returned an incomplete driver");
        return -1;
    }

    db_select_request_t req;
    memset(&req, 0, sizeof(req));
    req.sql              = sql;
    req.max_rows         = 2;
    req.fetch_array_size = 2;
    req.query_timeout    = query_timeout;
    req.bind_count       = 0;      /* Stage 5 - no binds         */
    req.bind_values      = NULL;
    req.text_lobs_inline = 0;

    db_select_cursor_t *cursor     = NULL;
    db_column_meta_t   *columns    = NULL;
    int                 col_count  = 0;
    int                 batch_size = 0;

    if (driver->select_open(ctx, &req, &cursor, &columns,
                            &col_count, &batch_size) != 0)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "select_open failed for sql=%s", sql);
        return -1;
    }

    int          rc    = -1;
    resultset_t *rs    = NULL;
    resultset_t *extra = NULL;
    int          rows  = 0;
    int          more  = 0;
    char         text[sizeof(rs->records[0].fields[0].value)];
    char        *p     = NULL;
    char        *end   = NULL;
    long long    v     = 0;

    if (col_count != 1)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "expected 1 column, query returned %d - sql=%s",
                     col_count, sql);
        goto Done;
    }

    if (driver->select_fetch_batch(cursor, &rs, &rows, NULL) != 0)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "select_fetch_batch failed for sql=%s", sql);
        goto Done;
    }

    if (rows == 1 &&
        driver->select_fetch_batch(cursor, &extra, &more, NULL) != 0)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "select_fetch_batch failed for sql=%s", sql);
        goto Done;
    }

    if (rows + more != 1 || !rs || rs->records[0].field_count < 1)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "expected exactly 1 row, query returned %s - sql=%s",
                     rows == 0 ? "none" : "more than 1", sql);
        goto Done;
    }

    snprintf(text, sizeof(text), "%s", rs->records[0].fields[0].value);
    for (p = text; *p == ' '; p++)
        ;

    errno = 0;
    v = strtoll(p, &end, 10);
    while (*end == ' ')
        end++;

    if (*p == '\0' || end == p || *end != '\0' || errno == ERANGE)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "value '%s' is not a whole number - sql=%s", text, sql);
        goto Done;
    }

    *out_value = v;
    rc = 0;

Done:
    if (extra) resultset_free(extra);
    if (rs)    resultset_free(rs);
    driver->select_close(cursor);
    free(columns);
    return rc;
}
