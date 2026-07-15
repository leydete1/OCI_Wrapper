/*
 * sql_dependency_extractor.c
 *
 * SQL Dependency Extractor - Implementation
 * ------------------------------------------
 * Parses a strictly-formatted SELECT statement and returns every
 * table/view and field reference in an OCI_DEPENDENCY_LIST.
 *
 * Parser design
 * -------------
 * Single-pass tokeniser operating on a normalised copy of the SQL
 * (whitespace collapsed, uppercased).  No heap allocation is used
 * during parsing - all output goes into the caller-supplied struct.
 *
 * Parse stages
 * ------------
 *   Stage 1  Normalise SQL (uppercase, collapse whitespace)
 *   Stage 2  Validate: must start with SELECT, no * wildcard,
 *            no subqueries, no UNION/INTERSECT/EXCEPT
 *   Stage 3  Locate SELECT ... FROM boundary
 *   Stage 4  Parse SELECT field list  -> deps->fields[]
 *   Stage 5  Locate FROM ... <end|WHERE|ORDER|GROUP|HAVING> boundary
 *   Stage 6  Parse FROM table list (comma-separated and JOIN clauses)
 *            -> deps->objects[]
 *   Stage 7  Cross-reference fields to objects via table_ref / alias
 *   Stage 8  Validate every field has a resolvable owner
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "sql_dependency_extractor.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  Internal logging macro                                              */
/*  Writes to ctx->sql_parser_logger when ctx is non-NULL, otherwise       */
/*  falls back to printf so the module works in unit-test contexts.    */
/* ------------------------------------------------------------------ */
#define DEP_LOG(ctx, level, fmt, ...) \
    do { \
        if ((ctx) && (ctx)->sql_parser_logger){ \
            logger_write((ctx)->sql_parser_logger, (level), \
                         "sql_dep", 0, (fmt), ##__VA_ARGS__); \
        }else if ((level) >= LOG_INFO) \
            printf("[sql_dep] " fmt "\n", ##__VA_ARGS__); \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define NORM_SQL_MAX   8192   /* normalised SQL buffer size            */
#define TOKEN_MAX       256   /* single token buffer size              */

/* ------------------------------------------------------------------ */
/*  Internal error helper - logs and returns -1                        */
/* ------------------------------------------------------------------ */
static int dep_fail(oci_context_t *ctx, const char *reason)
{
    DEP_LOG(ctx, LOG_ERROR,
            "extract_sql_dependencies FAILED: %s "
            "(rewrite the SQL or wrap in a stored procedure)",
            reason);
    return -1;
}

/* ================================================================== */
/*  Stage 1: Normalise                                                  */
/*  Uppercase, collapse all whitespace runs to single space,           */
/*  strip leading/trailing whitespace, remove trailing semicolon.      */
/* ================================================================== */
static int normalise_sql(const char *sql, char *out, size_t out_max)
{
    if (!sql || !out || out_max < 2) return -1;

    size_t wi = 0;
    int    in_space = 1;   /* treat leading whitespace as in-space    */

    for (size_t i = 0; sql[i] && wi < out_max - 1; i++)
    {
        char c = (char)toupper((unsigned char)sql[i]);

        if (isspace((unsigned char)sql[i]))
        {
            if (!in_space && wi < out_max - 1)
            {
                out[wi++] = ' ';
                in_space = 1;
            }
        }
        else
        {
            out[wi++] = c;
            in_space = 0;
        }
    }

    /* Strip trailing space */
    while (wi > 0 && out[wi - 1] == ' ') wi--;

    /* Strip trailing semicolon */
    if (wi > 0 && out[wi - 1] == ';') wi--;

    /* Strip trailing space again after semicolon removal */
    while (wi > 0 && out[wi - 1] == ' ') wi--;

    out[wi] = '\0';
    return 0;
}

/* ================================================================== */
/*  Utility: trim leading and trailing spaces in-place                 */
/* ================================================================== */
static void trim_token(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p == ' ') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int len = (int)strlen(s);
    while (len > 0 && s[len - 1] == ' ') { s[--len] = '\0'; }
}

/* ================================================================== */
/*  Utility: find the first occurrence of keyword as a whole word      */
/*  in src starting at offset.  Returns pointer or NULL.               */
/*  "Whole word" means not preceded or followed by an alphanumeric     */
/*  or underscore character.                                           */
/* ================================================================== */
static const char *find_keyword(const char *src, const char *kw)
{
    if (!src || !kw) return NULL;
    size_t klen = strlen(kw);

    const char *p = src;
    while ((p = strstr(p, kw)) != NULL)
    {
        /* Check character before */
        if (p > src)
        {
            char before = *(p - 1);
            if (isalnum((unsigned char)before) || before == '_')
            {
                p++;
                continue;
            }
        }
        /* Check character after */
        char after = *(p + klen);
        if (isalnum((unsigned char)after) || after == '_')
        {
            p++;
            continue;
        }
        return p;
    }
    return NULL;
}

/* ================================================================== */
/*  Utility: split a comma-separated token list into an array.         */
/*  Modifies buf in place (replaces commas with NUL).                  */
/*  Returns token count.                                               */
/* ================================================================== */
static int split_on_comma(char *buf, char **tokens, int max_tokens)
{
    int count = 0;
    char *p = buf;

    while (*p && count < max_tokens)
    {
        /* Skip leading spaces */
        while (*p == ' ') p++;
        if (!*p) break;

        tokens[count++] = p;

        /* Find next comma */
        char *comma = strchr(p, ',');
        if (!comma) break;

        *comma = '\0';
        p = comma + 1;
    }

    /* Trim each token */
    for (int i = 0; i < count; i++)
        trim_token(tokens[i]);

    return count;
}

/* ================================================================== */
/*  Stage 4: Parse one SELECT field token                              */
/*  Handles four formats (already uppercased, trimmed):                */
/*                                                                     */
/*  Normal (fully qualified):                                          */
/*    TABLE_REF.FIELD_NAME                                             */
/*    TABLE_REF.FIELD_NAME AS ALIAS                                    */
/*    TABLE_REF.FIELD_NAME ALIAS                                       */
/*                                                                     */
/*  Wildcard - all columns (expansion_type = SQL_FIELD_WILDCARD_ALL):  */
/*    *                                                                */
/*                                                                     */
/*  Wildcard - one table (expansion_type = SQL_FIELD_WILDCARD_TBL):   */
/*    TABLE_REF.*                                                      */
/*    TABLE_REF.* AS ALIAS  (alias ignored for wildcards)             */
/*                                                                     */
/*  Unqualified (expansion_type = SQL_FIELD_UNQUALIFIED):             */
/*    FIELD_NAME                                                        */
/*    FIELD_NAME AS ALIAS                                              */
/*    FIELD_NAME ALIAS                                                  */
/*    Only accepted when object_count == 1; table_ref is set to the   */
/*    single object's alias (or object_name if no alias).             */
/*    object_count is passed in via single_table_ref (NULL = multi).  */
/*                                                                     */
/*  Returns 0 on success, -1 if format is invalid.                    */
/* ================================================================== */
static int parse_field_token(const char    *token,
                               OCI_FIELD_REF *field,
                               int            pos,
                               const char    *single_table_ref,
                               oci_context_t *ctx)
{
    memset(field, 0, sizeof(*field));
    field->field_pos      = pos;
    field->expansion_type = SQL_FIELD_NORMAL;

    /* ---- Pure wildcard: bare * ---- */
    if (strcmp(token, "*") == 0)
    {
        DEP_LOG(ctx, LOG_INFO,
                "Field at position %d is SELECT * - "
                "marking for Phase B expansion", pos);
        field->field_name[0]  = '*';
        field->field_name[1]  = '\0';
        field->expansion_type = SQL_FIELD_WILDCARD_ALL;
        return 0;
    }

    /* ---- Reject functions: look for '(' ---- */
    if (strchr(token, '(') || strchr(token, ')'))
    {
        DEP_LOG(ctx, LOG_ERROR,
                "Field at position %d contains a function call: '%s'. "
                "Functions in SELECT are not supported. "
                "Wrap in a stored procedure or view.",
                pos, token);
        return -1;
    }

    const char *dot = strchr(token, '.');

    if (!dot)
    {
        /* ---- No dot: unqualified field ---- */
        if (!single_table_ref)
        {
            DEP_LOG(ctx, LOG_ERROR,
                    "Field at position %d is not qualified with a table "
                    "reference: '%s'. "
                    "Unqualified fields are only allowed when there is "
                    "exactly one table in the FROM clause.",
                    pos, token);
            return -1;
        }

        /* Single-table SQL: infer table_ref from the FROM clause */
        strncpy(field->table_ref, single_table_ref,
                sizeof(field->table_ref) - 1);
        field->expansion_type = SQL_FIELD_UNQUALIFIED;

        /* Parse FIELD_NAME [AS ALIAS] or FIELD_NAME ALIAS */
        const char *as_ptr = find_keyword(token, "AS");
        if (as_ptr)
        {
            size_t fn_len = (size_t)(as_ptr - token);
            while (fn_len > 0 && token[fn_len - 1] == ' ') fn_len--;
            if (fn_len == 0 || fn_len >= sizeof(field->field_name))
            {
                DEP_LOG(ctx, LOG_ERROR,
                        "Unqualified field at position %d has empty or "
                        "oversized name: '%s'.", pos, token);
                return -1;
            }
            memcpy(field->field_name, token, fn_len);
            field->field_name[fn_len] = '\0';
            const char *al = as_ptr + 2;
            while (*al == ' ') al++;
            if (*al)
                strncpy(field->field_alias, al,
                        sizeof(field->field_alias) - 1);
        }
        else
        {
            const char *space = strchr(token, ' ');
            if (space)
            {
                size_t fn_len = (size_t)(space - token);
                if (fn_len == 0 || fn_len >= sizeof(field->field_name))
                {
                    DEP_LOG(ctx, LOG_ERROR,
                            "Unqualified field at position %d has empty "
                            "or oversized name: '%s'.", pos, token);
                    return -1;
                }
                memcpy(field->field_name, token, fn_len);
                field->field_name[fn_len] = '\0';
                const char *al = space + 1;
                while (*al == ' ') al++;
                if (*al)
                    strncpy(field->field_alias, al,
                            sizeof(field->field_alias) - 1);
            }
            else
            {
                strncpy(field->field_name, token,
                        sizeof(field->field_name) - 1);
            }
        }

        trim_token(field->field_name);
        if (field->field_name[0] == '\0')
        {
            DEP_LOG(ctx, LOG_ERROR,
                    "Unqualified field at position %d has empty name: "
                    "'%s'.", pos, token);
            return -1;
        }

        DEP_LOG(ctx, LOG_INFO,
                "Field at position %d '%s' is unqualified - "
                "inferred table_ref='%s' (Phase B expansion)",
                pos, field->field_name, field->table_ref);
        return 0;
    }

    /* ---- Dot present: qualified or wildcard ---- */

    /* Extract table reference (before dot) */
    size_t ref_len = (size_t)(dot - token);
    if (ref_len == 0 || ref_len >= sizeof(field->table_ref))
    {
        DEP_LOG(ctx, LOG_ERROR,
                "Field at position %d has empty or oversized table "
                "reference: '%s'.", pos, token);
        return -1;
    }
    memcpy(field->table_ref, token, ref_len);
    field->table_ref[ref_len] = '\0';

    const char *after_dot = dot + 1;

    /* ---- TABLE.* wildcard ---- */
    if (after_dot[0] == '*' &&
        (after_dot[1] == '\0' || after_dot[1] == ' '))
    {
        DEP_LOG(ctx, LOG_INFO,
                "Field at position %d is %s.* - "
                "marking for Phase B expansion",
                pos, field->table_ref);
        field->field_name[0]  = '*';
        field->field_name[1]  = '\0';
        field->expansion_type = SQL_FIELD_WILDCARD_TBL;
        return 0;
    }

    if (*after_dot == '\0')
    {
        DEP_LOG(ctx, LOG_ERROR,
                "Field at position %d has no column name after '.': "
                "'%s'.", pos, token);
        return -1;
    }

    /* ---- Normal qualified field: TABLE.COLUMN [AS ALIAS] ---- */
    const char *as_ptr = find_keyword(after_dot, "AS");

    if (as_ptr)
    {
        size_t fn_len = (size_t)(as_ptr - after_dot);
        while (fn_len > 0 && after_dot[fn_len - 1] == ' ') fn_len--;

        if (fn_len == 0 || fn_len >= sizeof(field->field_name))
        {
            DEP_LOG(ctx, LOG_ERROR,
                    "Field at position %d has empty or oversized "
                    "column name: '%s'.", pos, token);
            return -1;
        }
        memcpy(field->field_name, after_dot, fn_len);
        field->field_name[fn_len] = '\0';

        const char *alias_start = as_ptr + 2;
        while (*alias_start == ' ') alias_start++;
        if (*alias_start)
            strncpy(field->field_alias, alias_start,
                    sizeof(field->field_alias) - 1);
    }
    else
    {
        const char *space = strchr(after_dot, ' ');
        if (space)
        {
            size_t fn_len = (size_t)(space - after_dot);
            if (fn_len == 0 || fn_len >= sizeof(field->field_name))
            {
                DEP_LOG(ctx, LOG_ERROR,
                        "Field at position %d has empty or oversized "
                        "column name: '%s'.", pos, token);
                return -1;
            }
            memcpy(field->field_name, after_dot, fn_len);
            field->field_name[fn_len] = '\0';

            const char *alias_start = space + 1;
            while (*alias_start == ' ') alias_start++;
            if (*alias_start)
                strncpy(field->field_alias, alias_start,
                        sizeof(field->field_alias) - 1);
        }
        else
        {
            strncpy(field->field_name, after_dot,
                    sizeof(field->field_name) - 1);
        }
    }

    trim_token(field->field_name);
    if (field->field_name[0] == '\0')
    {
        DEP_LOG(ctx, LOG_ERROR,
                "Field at position %d has empty column name: '%s'.",
                pos, token);
        return -1;
    }

    return 0;
}

/* ================================================================== */
/*  Stage 6: Parse one FROM-clause table token                         */
/*  Expected formats (already uppercased, trimmed):                   */
/*    TABLE_NAME                                                        */
/*    TABLE_NAME ALIAS                                                  */
/*    TABLE_NAME AS ALIAS                                               */
/*    OWNER.TABLE_NAME                                                  */
/*    OWNER.TABLE_NAME AS ALIAS                                         */
/*  Returns 0 on success, -1 if format is invalid.                    */
/* ================================================================== */
static int parse_table_token(const char     *token,
                               OCI_OBJECT_REF *obj,
                               oci_context_t  *ctx)
{
    memset(obj, 0, sizeof(*obj));
    obj->object_type = SQL_OBJ_TYPE_TABLE;
    obj->field_pos   = 0;   /* 0 = FROM-clause entry                  */

    if (!token || token[0] == '\0') return -1;

    /* Reject subquery fragments */
    if (strchr(token, '(') || strchr(token, ')'))
    {
        DEP_LOG(ctx, LOG_ERROR,
                "FROM clause contains a subquery or parenthesised "
                "expression: '%s'. Subqueries are not supported. "
                "Wrap in a view or stored procedure.", token);
        return -1;
    }

    /* Work on a mutable copy */
    char work[512] = {0};
    strncpy(work, token, sizeof(work) - 1);

    /* Check for alias: TABLE_NAME [AS] ALIAS */
    char *alias_start = NULL;
    char *as_ptr = (char *)find_keyword(work, "AS");

    if (as_ptr)
    {
        *as_ptr = '\0';
        alias_start = as_ptr + 2;
        while (*alias_start == ' ') alias_start++;
        trim_token(work);
    }
    else
    {
        /* Space-separated alias: TABLE_NAME ALIAS */
        char *space = strchr(work, ' ');
        if (space)
        {
            *space = '\0';
            alias_start = space + 1;
            while (*alias_start == ' ') alias_start++;
            trim_token(work);
        }
    }

    /* work now holds TABLE_NAME or OWNER.TABLE_NAME */
    char *dot = strchr(work, '.');
    if (dot)
    {
        /* OWNER.TABLE_NAME */
        size_t owner_len = (size_t)(dot - work);
        if (owner_len == 0 || owner_len >= sizeof(obj->owner))
        {
            DEP_LOG(ctx, LOG_ERROR,
                    "FROM clause table has invalid owner: '%s'.", token);
            return -1;
        }
        memcpy(obj->owner, work, owner_len);
        obj->owner[owner_len] = '\0';
        strncpy(obj->object_name, dot + 1, sizeof(obj->object_name) - 1);
    }
    else
    {
        /* No owner specified - leave owner empty; caller resolves */
        strncpy(obj->object_name, work, sizeof(obj->object_name) - 1);
    }

    trim_token(obj->object_name);
    if (obj->object_name[0] == '\0')
    {
        DEP_LOG(ctx, LOG_ERROR,
                "FROM clause has empty table name in token: '%s'.",
                token);
        return -1;
    }

    /* Copy alias */
    if (alias_start && *alias_start)
        strncpy(obj->alias, alias_start, sizeof(obj->alias) - 1);

    return 0;
}

/* ================================================================== */
/*  Stage 6 helper: strip JOIN keywords from a FROM segment            */
/*  Converts "T1, T2 INNER JOIN T3 ON ..." into comma-friendly form.  */
/*  Replaces JOIN ... ON ... with comma-separated table refs.          */
/* ================================================================== */
static void extract_join_tables(const char     *from_seg,
                                 OCI_OBJECT_REF *objects,
                                 int            *obj_count,
                                 int             max_objects,
                                 oci_context_t  *ctx)
{
    /*
     * Strategy: scan the FROM segment for JOIN keywords.
     * Each JOIN introduces one more table.  The ON clause that follows
     * is discarded (we only need the table name, not the join condition).
     *
     * Supported JOIN types: JOIN, INNER JOIN, LEFT JOIN, LEFT OUTER JOIN,
     * RIGHT JOIN, RIGHT OUTER JOIN, CROSS JOIN, FULL JOIN, FULL OUTER JOIN.
     * All are normalised to just "the token before ON".
     */

    /* Work on a mutable copy */
    char work[NORM_SQL_MAX] = {0};
    strncpy(work, from_seg, sizeof(work) - 1);

    /* First pass: collect the base table (everything before the first JOIN) */
    char *first_join = NULL;
    const char *join_kws[] = {
        "INNER JOIN", "LEFT OUTER JOIN", "RIGHT OUTER JOIN",
        "FULL OUTER JOIN", "CROSS JOIN", "LEFT JOIN",
        "RIGHT JOIN", "FULL JOIN", "JOIN", NULL
    };

    /* Find the earliest JOIN keyword position */
    char *earliest = NULL;
    for (int k = 0; join_kws[k]; k++)
    {
        char *found = (char *)find_keyword(work, join_kws[k]);
        if (found && (!earliest || found < earliest))
            earliest = found;
    }
    first_join = earliest;

    if (!first_join)
    {
        /* No JOINs at all - handle as plain comma list */
        char copy[NORM_SQL_MAX] = {0};
        strncpy(copy, from_seg, sizeof(copy) - 1);
        char *tokens[SQL_DEP_MAX_OBJECTS];
        int n = split_on_comma(copy, tokens, SQL_DEP_MAX_OBJECTS);
        for (int i = 0; i < n && *obj_count < max_objects; i++)
        {
            if (tokens[i] && tokens[i][0])
            {
                if (parse_table_token(tokens[i],
                                       &objects[*obj_count], ctx) == 0)
                    (*obj_count)++;
            }
        }
        return;
    }

    /* Extract the base table list (before first JOIN) */
    char base_part[NORM_SQL_MAX] = {0};
    size_t base_len = (size_t)(first_join - work);
    if (base_len > 0)
    {
        memcpy(base_part, work, base_len);
        base_part[base_len] = '\0';
        trim_token(base_part);

        char *tokens[SQL_DEP_MAX_OBJECTS];
        int n = split_on_comma(base_part, tokens, SQL_DEP_MAX_OBJECTS);
        for (int i = 0; i < n && *obj_count < max_objects; i++)
        {
            if (tokens[i] && tokens[i][0])
            {
                if (parse_table_token(tokens[i],
                                       &objects[*obj_count], ctx) == 0)
                    (*obj_count)++;
            }
        }
    }

    /* Now scan for each JOIN ... [ON|USING] block and extract table */
    char *cursor = first_join;
    while (cursor && *cursor)
    {
        /* Find which JOIN keyword we are at */
        const char *matched_kw = NULL;
        size_t      matched_len = 0;

        for (int k = 0; join_kws[k]; k++)
        {
            size_t kwlen = strlen(join_kws[k]);
            if (strncmp(cursor, join_kws[k], kwlen) == 0)
            {
                char after = cursor[kwlen];
                if (after == ' ' || after == '\0')
                {
                    matched_kw  = join_kws[k];
                    matched_len = kwlen;
                    break;
                }
            }
        }

        if (!matched_kw) break;

        /* Skip past the JOIN keyword */
        cursor += matched_len;
        while (*cursor == ' ') cursor++;

        /* Table token ends at: ON, USING, next JOIN keyword, or end */
        char *table_end = cursor;
        const char *stop_kws[] = {
            "INNER JOIN", "LEFT OUTER JOIN", "RIGHT OUTER JOIN",
            "FULL OUTER JOIN", "CROSS JOIN", "LEFT JOIN",
            "RIGHT JOIN", "FULL JOIN", "JOIN",
            "ON", "USING", "WHERE", "GROUP", "ORDER", "HAVING", NULL
        };

        char *earliest_stop = NULL;
        for (int k = 0; stop_kws[k]; k++)
        {
            char *found = (char *)find_keyword(cursor, stop_kws[k]);
            if (found && (!earliest_stop || found < earliest_stop))
                earliest_stop = found;
        }

        char table_tok[512] = {0};
        if (earliest_stop)
        {
            size_t tlen = (size_t)(earliest_stop - cursor);
            if (tlen > 0 && tlen < sizeof(table_tok))
            {
                memcpy(table_tok, cursor, tlen);
                table_tok[tlen] = '\0';
            }
            trim_token(table_tok);

            /* Advance cursor to next JOIN or end */
            cursor = earliest_stop;

            /* If we stopped at ON/USING, skip to next JOIN */
            if (strncmp(cursor, "ON", 2) == 0 ||
                strncmp(cursor, "USING", 5) == 0)
            {
                /* Skip to next JOIN keyword */
                char *next_join = NULL;
                for (int k = 0; join_kws[k]; k++)
                {
                    char *found = (char *)find_keyword(cursor, join_kws[k]);
                    if (found && (!next_join || found < next_join))
                        next_join = found;
                }
                cursor = next_join ? next_join : NULL;
            }
        }
        else
        {
            /* No stop keyword - rest is the table token */
            strncpy(table_tok, cursor, sizeof(table_tok) - 1);
            trim_token(table_tok);
            cursor = NULL;
        }

        if (table_tok[0] && *obj_count < max_objects)
        {
            if (parse_table_token(table_tok,
                                   &objects[*obj_count], ctx) == 0)
                (*obj_count)++;
        }
    }
}

/* ================================================================== */
/*  Stage 7: resolve field table_ref to a fully qualified owner        */
/*  Maps field.table_ref (which may be a table alias) back to the     */
/*  corresponding OCI_OBJECT_REF and copies the owner.                */
/* ================================================================== */
static int resolve_field_owners(OCI_DEPENDENCY_LIST *deps,
                                 oci_context_t       *ctx)
{
    for (int f = 0; f < deps->field_count; f++)
    {
        OCI_FIELD_REF *field = &deps->fields[f];

        /* Wildcard and unqualified fields are resolved in Phase B
         * after OCI describe - skip cross-reference check here.    */
        if (field->expansion_type == SQL_FIELD_WILDCARD_ALL ||
            field->expansion_type == SQL_FIELD_UNQUALIFIED)
        {
            DEP_LOG(ctx, LOG_DEBUG,
                    "Field pos=%d '%s' skipped in cross-reference "
                    "(expansion_type=%d - resolved in Phase B)",
                    field->field_pos,
                    field->field_name,
                    field->expansion_type);
            continue;
        }

        /* TABLE.* wildcard: validate table_ref exists in FROM clause */
        if (field->expansion_type == SQL_FIELD_WILDCARD_TBL)
        {
            int resolved = 0;
            for (int o = 0; o < deps->object_count; o++)
            {
                OCI_OBJECT_REF *obj = &deps->objects[o];
                int alias_match = (obj->alias[0] != '\0' &&
                                   strcasecmp(field->table_ref,
                                              obj->alias) == 0);
                int name_match  = (strcasecmp(field->table_ref,
                                              obj->object_name) == 0);
                if (alias_match || name_match) { resolved = 1; break; }
            }
            if (!resolved)
            {
                DEP_LOG(ctx, LOG_ERROR,
                        "Wildcard %s.* at position %d references a "
                        "table not in the FROM clause.",
                        field->table_ref, field->field_pos);
                return -1;   /* stop immediately on first failure     */
            }
            continue;
        }

        /* Normal fully qualified field: validate table_ref */
        int resolved = 0;

        for (int o = 0; o < deps->object_count; o++)
        {
            OCI_OBJECT_REF *obj = &deps->objects[o];

            int alias_match = (obj->alias[0] != '\0' &&
                               strcasecmp(field->table_ref,
                                          obj->alias) == 0);
            int name_match  = (strcasecmp(field->table_ref,
                                          obj->object_name) == 0);

            if (alias_match || name_match)
            {
                DEP_LOG(ctx, LOG_DEBUG,
                        "Field pos=%d '%s.%s' resolved to object "
                        "'%s.%s'",
                        field->field_pos,
                        field->table_ref,
                        field->field_name,
                        obj->owner,
                        obj->object_name);
                resolved = 1;
                break;
            }
        }

        if (!resolved)
        {
            DEP_LOG(ctx, LOG_ERROR,
                    "Field at position %d references table '%s' which "
                    "does not appear in the FROM clause.",
                    field->field_pos, field->table_ref);
            return -1;   /* stop immediately on first failure         */
        }
    }

    return 0;   /* all fields resolved                                */
}

/* ================================================================== */
/*  extract_sql_dependencies  -  main entry point                      */
/* ================================================================== */
int extract_sql_dependencies(const char         *sql,
                               OCI_DEPENDENCY_LIST *deps,
                               oci_context_t       *ctx)
{
    DEP_LOG(ctx, LOG_INFO,
            "Entering extract_sql_dependencies sql='%.120s'", sql);

    if (!sql || !deps)
        return dep_fail(ctx, "NULL sql or deps pointer");

    memset(deps, 0, sizeof(*deps));

    /* ================================================================
     *  Stage 1: Normalise
     * ================================================================ */
    char norm[NORM_SQL_MAX] = {0};
    if (normalise_sql(sql, norm, sizeof(norm)) != 0)
        return dep_fail(ctx, "SQL too long or normalisation failed");

    DEP_LOG(ctx, LOG_DEBUG, "Normalised SQL: '%s'", norm);

    /* ================================================================
     *  Stage 2: Structural validation
     * ================================================================ */

    /* Must start with SELECT */
    if (strncmp(norm, "SELECT ", 7) != 0)
        return dep_fail(ctx,
            "SQL does not begin with SELECT. "
            "Only SELECT statements are supported.");

    /* Must contain FROM */
    if (!find_keyword(norm, "FROM"))
        return dep_fail(ctx,
            "SQL has no FROM clause.");

    /* Reject UNION / INTERSECT / EXCEPT */
    if (find_keyword(norm, "UNION")     ||
        find_keyword(norm, "INTERSECT") ||
        find_keyword(norm, "EXCEPT"))
        return dep_fail(ctx,
            "UNION, INTERSECT and EXCEPT are not supported. "
            "Wrap in a stored procedure or view.");

    /* ================================================================
     *  Stage 3: Locate SELECT...FROM boundary
     * ================================================================ */
    const char *from_ptr = find_keyword(norm, "FROM");
    if (!from_ptr)
        return dep_fail(ctx, "FROM clause not found after normalisation.");

    /* SELECT clause is between "SELECT " and " FROM" */
    const char *select_start = norm + 7;   /* skip "SELECT " */
    size_t select_len = (size_t)(from_ptr - select_start);

    /* Trim trailing space from select clause length */
    while (select_len > 0 &&
           select_start[select_len - 1] == ' ')
        select_len--;

    if (select_len == 0)
        return dep_fail(ctx, "SELECT clause is empty.");

    char select_clause[NORM_SQL_MAX] = {0};
    if (select_len >= sizeof(select_clause))
        return dep_fail(ctx, "SELECT clause too long.");
    memcpy(select_clause, select_start, select_len);
    select_clause[select_len] = '\0';

    DEP_LOG(ctx, LOG_DEBUG, "SELECT clause: '%s'", select_clause);

    /* ================================================================
     *  Stage 5: Locate FROM clause extent
     *  Parse FROM before fields so we know object_count.
     *  This lets parse_field_token infer table_ref for unqualified
     *  fields when there is exactly one FROM-clause object.
     *  FROM ends at: WHERE, GROUP BY, ORDER BY, HAVING, or end of SQL
     * ================================================================ */
    const char *from_body_start = from_ptr + 4;   /* skip "FROM" */
    while (*from_body_start == ' ') from_body_start++;

    const char *from_end = NULL;
    const char *stop_kws[] = {
        "WHERE", "GROUP BY", "ORDER BY", "HAVING", NULL
    };

    for (int k = 0; stop_kws[k]; k++)
    {
        const char *found = find_keyword(from_body_start, stop_kws[k]);
        if (found && (!from_end || found < from_end))
            from_end = found;
    }

    char from_clause[NORM_SQL_MAX] = {0};
    if (from_end)
    {
        size_t from_len = (size_t)(from_end - from_body_start);
        while (from_len > 0 &&
               from_body_start[from_len - 1] == ' ')
            from_len--;
        if (from_len >= sizeof(from_clause))
            return dep_fail(ctx, "FROM clause too long.");
        memcpy(from_clause, from_body_start, from_len);
        from_clause[from_len] = '\0';
    }
    else
    {
        strncpy(from_clause, from_body_start, sizeof(from_clause) - 1);
        trim_token(from_clause);
    }

    if (from_clause[0] == '\0')
        return dep_fail(ctx, "FROM clause is empty.");

    DEP_LOG(ctx, LOG_DEBUG, "FROM clause: '%s'", from_clause);

    /* ================================================================
     *  Stage 6: Parse FROM table list (handles commas and JOINs)
     * ================================================================ */
    extract_join_tables(from_clause,
                         deps->objects,
                         &deps->object_count,
                         SQL_DEP_MAX_OBJECTS,
                         ctx);

    if (deps->object_count == 0)
        return dep_fail(ctx, "No table references found in FROM clause.");

    DEP_LOG(ctx, LOG_INFO, "FROM clause contains %d object(s)",
            deps->object_count);

    for (int i = 0; i < deps->object_count; i++)
    {
        DEP_LOG(ctx, LOG_INFO,
                "  Object[%d]: owner='%s' name='%s' alias='%s'",
                i,
                deps->objects[i].owner[0]
                    ? deps->objects[i].owner : "(none)",
                deps->objects[i].object_name,
                deps->objects[i].alias[0]
                    ? deps->objects[i].alias : "(none)");
    }

    /* ================================================================
     *  Stage 4: Parse SELECT field list
     *
     *  single_table_ref is the alias (or object_name if no alias) of
     *  the sole FROM-clause object.  It is passed to parse_field_token
     *  so unqualified fields can have their table_ref inferred.
     *  When object_count > 1 it is NULL and unqualified fields fail.
     * ================================================================ */
    const char *single_table_ref = NULL;
    if (deps->object_count == 1)
    {
        single_table_ref = deps->objects[0].alias[0]
                           ? deps->objects[0].alias
                           : deps->objects[0].object_name;
    }

    char sel_work[NORM_SQL_MAX] = {0};
    strncpy(sel_work, select_clause, sizeof(sel_work) - 1);

    char *field_tokens[SQL_DEP_MAX_FIELDS];
    int   field_token_count = split_on_comma(sel_work,
                                              field_tokens,
                                              SQL_DEP_MAX_FIELDS);

    if (field_token_count == 0)
        return dep_fail(ctx, "No fields found in SELECT clause.");

    DEP_LOG(ctx, LOG_INFO, "SELECT clause contains %d field token(s)",
            field_token_count);

    for (int i = 0; i < field_token_count; i++)
    {
        if (!field_tokens[i] || field_tokens[i][0] == '\0') continue;

        OCI_FIELD_REF field;
        if (parse_field_token(field_tokens[i], &field, i + 1,
                              single_table_ref, ctx) != 0)
            return dep_fail(ctx,
                "SELECT clause field does not meet format requirements.");

        if (deps->field_count >= SQL_DEP_MAX_FIELDS)
            return dep_fail(ctx, "Too many fields in SELECT clause.");

        /* Track whether any field needs Phase B expansion */
        if (field.expansion_type != SQL_FIELD_NORMAL)
            deps->needs_expansion = 1;

        deps->fields[deps->field_count++] = field;

        DEP_LOG(ctx, LOG_INFO,
                "  Field[%d]: table_ref='%s' field='%s' alias='%s'%s",
                i + 1,
                field.table_ref[0] ? field.table_ref : "(inferred)",
                field.field_name,
                field.field_alias[0] ? field.field_alias : "(none)",
                field.expansion_type ? " [EXPAND]" : "");
    }

    /* ================================================================
     *  Stage 7: Cross-reference fields to objects
     *  Skip wildcard and unqualified entries - those are resolved in
     *  Phase B after OCI describe.  Only validate fully qualified
     *  fields that have a real table_ref to check.
     * ================================================================ */
    /*if (resolve_field_owners(deps, ctx) != 0)
        return dep_fail(ctx,
            "One or more SELECT fields reference a table or alias "
            "not found in the FROM clause.");
     */
    /* ================================================================
     *  Stage 8: Summary
     * ================================================================ */
    DEP_LOG(ctx, LOG_INFO,
            "extract_sql_dependencies OK: objects=%d fields=%d "
            "needs_expansion=%d",
            deps->object_count, deps->field_count,
            deps->needs_expansion);

    return 0;
}

/* ================================================================== */
/*  sql_dep_dump                                                        */
/* ================================================================== */
void sql_dep_dump(const OCI_DEPENDENCY_LIST *deps,
                  oci_context_t             *ctx)
{
    if (!deps) return;

    DEP_LOG(ctx, LOG_INFO,
            "=== OCI_DEPENDENCY_LIST dump ===");
    DEP_LOG(ctx, LOG_INFO,
            "  object_count=%d  field_count=%d  needs_expansion=%d",
            deps->object_count, deps->field_count, deps->needs_expansion);

    DEP_LOG(ctx, LOG_INFO, "  --- FROM objects ---");
    for (int i = 0; i < deps->object_count; i++)
    {
        const OCI_OBJECT_REF *o = &deps->objects[i];
        DEP_LOG(ctx, LOG_INFO,
                "  [%d] owner='%s' name='%s' alias='%s' type=%d",
                i,
                o->owner[0]       ? o->owner       : "(none)",
                o->object_name[0] ? o->object_name : "(empty)",
                o->alias[0]       ? o->alias        : "(none)",
                o->object_type);
    }

    DEP_LOG(ctx, LOG_INFO, "  --- SELECT fields ---");
    for (int i = 0; i < deps->field_count; i++)
    {
        const OCI_FIELD_REF *f = &deps->fields[i];
        DEP_LOG(ctx, LOG_INFO,
                "  [%d] pos=%d table_ref='%s' field='%s' alias='%s'%s",
                i,
                f->field_pos,
                f->table_ref[0] ? f->table_ref : "(inferred)",
                f->field_name,
                f->field_alias[0] ? f->field_alias : "(none)",
                f->expansion_type ? " [EXPAND]" : "");
    }

    DEP_LOG(ctx, LOG_INFO, "=== end dump ===");
}
