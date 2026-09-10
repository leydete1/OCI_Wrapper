#include <ctype.h>
#include <string.h>
#include <string_utils.h>
#include "OCI_Connection.h"   /* oci_context_t */
#include "logger.h"

void sanitize_sql(char *sql)
{
    if (!sql) return;

    char *src = sql;
    char *dst = sql;

    while (*src)
    {
        unsigned char c = (unsigned char)*src;

        /* Remove UTF-8 NBSP sequence C2 A0 */
        if (c == 0xC2 && (unsigned char)*(src+1) == 0xA0)
        {
            src += 2;
            *dst++ = ' ';
            continue;
        }

        /* Convert tabs/newlines to space */
        if (c == '\t' || c == '\n' || c == '\r')
        {
            *dst++ = ' ';
            src++;
            continue;
        }

        *dst++ = *src++;
    }

    *dst = '\0';

    /* Trim leading spaces */
    char *start = sql;
    while (*start && isspace((unsigned char)*start))
        start++;

    if (start != sql)
        memmove(sql, start, strlen(start) + 1);

    /* Trim trailing spaces */
    int len = strlen(sql);
    while (len > 0 && isspace((unsigned char)sql[len - 1]))
    {
        sql[len - 1] = '\0';
        len--;
    }
}

/*
 * trim_sql_inplace()
 *
 * Relocated from OCI_Execute_Query_Module.c when that module was removed
 * (execute_query() was dead, but this helper is still called by
 * execute_query_batch()). Behaviour unchanged.
 */
void trim_sql_inplace(char *str, oci_context_t *ctx)
{
    if (!str) return;

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "In trim_sql_inplace str=%s", str);

    unsigned char *s = (unsigned char*)str;
    /* Skip leading whitespace + NBSP */
    while (*s && ((*s <= 0x20) || (*s == 0xC2 && *(s+1) == 0xA0))) {
        if (*s == 0xC2 && *(s+1) == 0xA0) s++;  /* skip extra byte */
        s++;
    }

    unsigned char *start = s;
    unsigned char *end = (unsigned char*)str + strlen(str) - 1;

    while (end >= start) {
        if (*end <= 0x20) {
            end--;
        } else if (end > start && *(end-1) == 0xC2 && *end == 0xA0) {
            end -= 2;
        } else if (*end == 0xA0) {  /* fallback */
            end--;
        } else break;
    }

    size_t len = end - start + 1;
    memmove(str, start, len);
    str[len] = '\0';
    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "Leaving trim_sql_inplace str=%s", str);
}
