/*
 * db_type_class.c
 *
 * See db_type_class.h. Oracle dialect extraction, Stage 1 (2026-09-30);
 * extended in Stage 4b (2026-10-06). SQL Server names added in
 * driver_mssql Stage 3 (2026-10-19).
 */

#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "db_type_class.h"

typedef struct {
    const char       *name;
    db_type_class_t   cls;
} type_map_t;

/* Base names only - "(n)", "(p,s)" and trailing qualifiers such as
 * "WITH TIME ZONE" are stripped before lookup (see base_name_len()).
 * INTERVAL is not listed: its class depends on the qualifier, see
 * db_type_class(). */
static const type_map_t TYPE_MAP[] = {
    /* ---- Oracle ---- */
    { "VARCHAR2",      DB_TYPE_CLASS_STRING    },
    { "NVARCHAR2",     DB_TYPE_CLASS_STRING    },
    { "VARCHAR",       DB_TYPE_CLASS_STRING    },
    { "CHAR",          DB_TYPE_CLASS_STRING    },
    { "NCHAR",         DB_TYPE_CLASS_STRING    },

    { "ROWID",         DB_TYPE_CLASS_ROWID     },   /* STRING before 4b */
    { "UROWID",        DB_TYPE_CLASS_ROWID     },   /* STRING before 4b */

    { "NUMBER",        DB_TYPE_CLASS_NUMERIC   },
    { "FLOAT",         DB_TYPE_CLASS_NUMERIC   },
    { "BINARY_FLOAT",  DB_TYPE_CLASS_NUMERIC   },
    { "BINARY_DOUBLE", DB_TYPE_CLASS_NUMERIC   },
    { "INTEGER",       DB_TYPE_CLASS_NUMERIC   },
    { "INT",           DB_TYPE_CLASS_NUMERIC   },
    { "SMALLINT",      DB_TYPE_CLASS_NUMERIC   },
    { "DECIMAL",       DB_TYPE_CLASS_NUMERIC   },
    { "NUMERIC",       DB_TYPE_CLASS_NUMERIC   },
    { "REAL",          DB_TYPE_CLASS_NUMERIC   },   /* 4b */
    { "DOUBLE",        DB_TYPE_CLASS_NUMERIC   },   /* 4b - "DOUBLE PRECISION" */

    { "DATE",          DB_TYPE_CLASS_DATE      },
    { "TIMESTAMP",     DB_TYPE_CLASS_TIMESTAMP },

    { "RAW",           DB_TYPE_CLASS_BINARY    },   /* 4b */

    { "BLOB",          DB_TYPE_CLASS_LOB       },
    { "CLOB",          DB_TYPE_CLASS_LOB       },
    { "NCLOB",         DB_TYPE_CLASS_LOB       },
    { "BFILE",         DB_TYPE_CLASS_LOB       },

    /* ---- SQL Server (driver_mssql Stage 3, 2026-10-19) ----
     * Names as driver_mssql's describe_table reports them. CHAR,
     * VARCHAR, NCHAR, INT, SMALLINT, DECIMAL, NUMERIC, FLOAT, REAL and
     * DATE are spelled as Oracle's and are listed above. A "(MAX)"
     * suffix makes any of them LOB - see db_type_class(). SQL Server's
     * own "timestamp" is rowversion; describe_table reports it as
     * ROWVERSION, which is not listed (OTHER). */
    { "NVARCHAR",         DB_TYPE_CLASS_STRING    },
    { "UNIQUEIDENTIFIER", DB_TYPE_CLASS_STRING    },
    { "TIME",             DB_TYPE_CLASS_STRING    },   /* time of day only -
                                                          not a date      */
    { "BIGINT",           DB_TYPE_CLASS_NUMERIC   },
    { "TINYINT",          DB_TYPE_CLASS_NUMERIC   },
    { "BIT",              DB_TYPE_CLASS_NUMERIC   },
    { "MONEY",            DB_TYPE_CLASS_NUMERIC   },
    { "SMALLMONEY",       DB_TYPE_CLASS_NUMERIC   },
    { "SMALLDATETIME",    DB_TYPE_CLASS_DATE      },
    { "DATETIME",         DB_TYPE_CLASS_TIMESTAMP },
    { "DATETIME2",        DB_TYPE_CLASS_TIMESTAMP },
    { "DATETIMEOFFSET",   DB_TYPE_CLASS_TIMESTAMP },
    { "BINARY",           DB_TYPE_CLASS_BINARY    },
    { "VARBINARY",        DB_TYPE_CLASS_BINARY    },
    { "TEXT",             DB_TYPE_CLASS_LOB       },
    { "NTEXT",            DB_TYPE_CLASS_LOB       },
    { "IMAGE",            DB_TYPE_CLASS_LOB       },
    { "XML",              DB_TYPE_CLASS_LOB       },   /* LOB class, but not
                                                          db_type_lob_kind():
                                                          binds as text  */
};

/* Length of the leading type word: stops at '(' or whitespace, so
 * "TIMESTAMP(6) WITH TIME ZONE" -> "TIMESTAMP", "NUMBER(10,2)" -> "NUMBER".
 * Underscores are part of the word (BINARY_FLOAT). */
static size_t base_name_len(const char *s)
{
    size_t n = 0;
    while (s[n] && s[n] != '(' && !isspace((unsigned char)s[n]))
        n++;
    return n;
}

/* Case-insensitive "does s contain word". */
static int contains_ci(const char *s, const char *word)
{
    size_t wl = strlen(word);
    for (; *s; s++)
        if (strncasecmp(s, word, wl) == 0)
            return 1;
    return 0;
}

static int base_is(const char *s, size_t n, const char *name)
{
    return strlen(name) == n && strncasecmp(name, s, n) == 0;
}

/* driver_mssql Stage 3: "(MAX)" straight after the base name, spaces
 * allowed - SQL Server's VARCHAR(MAX), NVARCHAR(MAX), VARBINARY(MAX). */
static int has_max_suffix(const char *after_base)
{
    const char *p = after_base;
    while (isspace((unsigned char)*p)) p++;
    if (*p != '(') return 0;
    p++;
    while (isspace((unsigned char)*p)) p++;
    if (strncasecmp(p, "MAX", 3) != 0) return 0;
    p += 3;
    while (isspace((unsigned char)*p)) p++;
    return *p == ')';
}

db_type_class_t db_type_class(const char *type_name)
{
    if (!type_name) return DB_TYPE_CLASS_OTHER;

    while (isspace((unsigned char)*type_name)) type_name++;

    size_t n = base_name_len(type_name);
    if (n == 0) return DB_TYPE_CLASS_OTHER;

    /* SQL Server "(MAX)" (driver_mssql Stage 3): up to 2 GB, read and
     * written as a LOB whatever the base name's own class. */
    if (has_max_suffix(type_name + n)) return DB_TYPE_CLASS_LOB;

    /* INTERVAL YEAR(p) TO MONTH / INTERVAL DAY(p) TO SECOND(s): the
     * class is in the qualifier. MONTH is checked first, matching the
     * order of the tests this replaces in Insert_Validate_Module.c. */
    if (base_is(type_name, n, "INTERVAL"))
    {
        if (contains_ci(type_name + n, "MONTH"))  return DB_TYPE_CLASS_INTERVAL_YM;
        if (contains_ci(type_name + n, "SECOND")) return DB_TYPE_CLASS_INTERVAL_DS;
        return DB_TYPE_CLASS_OTHER;
    }

    for (size_t i = 0; i < sizeof(TYPE_MAP) / sizeof(TYPE_MAP[0]); i++)
    {
        if (base_is(type_name, n, TYPE_MAP[i].name))
            return TYPE_MAP[i].cls;
    }
    return DB_TYPE_CLASS_OTHER;
}

const char *db_type_class_name(db_type_class_t cls)
{
    switch (cls)
    {
        case DB_TYPE_CLASS_STRING:      return "STRING";
        case DB_TYPE_CLASS_NUMERIC:     return "NUMERIC";
        case DB_TYPE_CLASS_DATE:        return "DATE";
        case DB_TYPE_CLASS_TIMESTAMP:   return "TIMESTAMP";
        case DB_TYPE_CLASS_LOB:         return "LOB";
        case DB_TYPE_CLASS_INTERVAL_YM: return "INTERVAL_YM";
        case DB_TYPE_CLASS_INTERVAL_DS: return "INTERVAL_DS";
        case DB_TYPE_CLASS_BINARY:      return "BINARY";
        case DB_TYPE_CLASS_ROWID:       return "ROWID";
        case DB_TYPE_CLASS_OTHER:
        default:                        return "OTHER";
    }
}

db_lob_kind_t db_type_lob_kind(const char *type_name)
{
    if (!type_name) return DB_LOB_NONE;

    while (isspace((unsigned char)*type_name)) type_name++;

    size_t n = base_name_len(type_name);
    if (n == 0) return DB_LOB_NONE;

    /* ---- Oracle ---- */
    if (base_is(type_name, n, "BLOB"))  return DB_LOB_BINARY;
    if (base_is(type_name, n, "CLOB"))  return DB_LOB_TEXT;
    if (base_is(type_name, n, "NCLOB")) return DB_LOB_TEXT;

    /* ---- SQL Server - the columns driver_mssql's lob_placeholder()
     * gives 0x. Since Stage 4 (U6) its text LOBs are bound in the
     * statement instead (db_type_text_bound()), so none is TEXT. ---- */
    if (has_max_suffix(type_name + n) && base_is(type_name, n, "VARBINARY"))
        return DB_LOB_BINARY;
    if (base_is(type_name, n, "IMAGE")) return DB_LOB_BINARY;

    return DB_LOB_NONE;   /* includes BFILE and SQL Server XML - see
                             db_type_class.h */
}

int db_type_text_bound(const char *type_name)
{
    if (!type_name) return 0;

    while (isspace((unsigned char)*type_name)) type_name++;

    size_t n = base_name_len(type_name);
    if (n == 0) return 0;

    /* ---- SQL Server (driver_mssql Stage 4, U6) - Oracle has none ---- */
    if (has_max_suffix(type_name + n))
        return base_is(type_name, n, "VARCHAR") || base_is(type_name, n, "NVARCHAR");
    return base_is(type_name, n, "TEXT") || base_is(type_name, n, "NTEXT") ||
           base_is(type_name, n, "XML");
}
