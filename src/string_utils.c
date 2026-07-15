#include <ctype.h>
#include <string.h>
#include <string_utils.h>

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
