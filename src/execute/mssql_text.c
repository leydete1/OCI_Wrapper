/*
 * mssql_text.c   (driver_mssql Stage 5, Fix 1, 2026-10-21)
 *
 * See mssql_text.h. Never includes an Oracle header.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mssql_text.h"

/* UTF-8 to a NUL-terminated SQLWCHAR (UTF-16) string; caller frees. */
static SQLWCHAR *utf8_to_wide(const char *s)
{
    size_t    n = strlen(s), k = 0;
    SQLWCHAR *w = malloc((n + 1) * 2 * sizeof(SQLWCHAR));   /* worst case */
    if (!w) return NULL;
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < n)
    {
        uint32_t cp;
        size_t   len;
        unsigned c = p[i];
        if (c < 0x80)                { cp = c;        len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else                         { cp = 0xFFFD;   len = 1; }
        if (len > 1)
        {
            if (i + len > n) { cp = 0xFFFD; len = 1; }
            else
                for (size_t j = 1; j < len; j++)
                {
                    if ((p[i + j] & 0xC0) != 0x80) { cp = 0xFFFD; len = 1; break; }
                    cp = (cp << 6) | (p[i + j] & 0x3F);
                }
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            w[k++] = (SQLWCHAR)(0xD800 + (cp >> 10));
            w[k++] = (SQLWCHAR)(0xDC00 + (cp & 0x3FF));
        }
        else
            w[k++] = (SQLWCHAR)cp;
        i += len;
    }
    w[k] = 0;
    return w;
}

SQLRETURN mssql_prepare_utf8(SQLHSTMT stmt, const char *sql)
{
    SQLWCHAR *w = utf8_to_wide(sql ? sql : "");
    if (!w) return SQL_ERROR;
    SQLRETURN r = SQLPrepareW(stmt, w, SQL_NTS);
    free(w);
    return r;
}

SQLRETURN mssql_exec_direct_utf8(SQLHSTMT stmt, const char *sql)
{
    SQLWCHAR *w = utf8_to_wide(sql ? sql : "");
    if (!w) return SQL_ERROR;
    SQLRETURN r = SQLExecDirectW(stmt, w, SQL_NTS);
    free(w);
    return r;
}
