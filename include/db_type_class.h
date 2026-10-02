/*
 * db_type_class.h
 *
 * Oracle dialect extraction, Stage 1 (2026-09-30).
 *
 * Maps a column's type NAME, as the metadata layer reports it
 * (col_metadata_t.data_type, e.g. "VARCHAR2", "TIMESTAMP(6) WITH TIME
 * ZONE"), to a small vendor-neutral class. Core code compares classes,
 * never vendor type names - so a DATE check in Level 2 does not need
 * to know that Oracle spells its types differently from SQL Server.
 *
 * Today only Oracle's names are listed. driver_mssql adds its own
 * names here (DATETIME2, NVARCHAR, ...) when it arrives; nothing that
 * calls db_type_class() has to change for that.
 *
 * Matching is case-insensitive and ignores any "(n)" / "(p,s)" suffix
 * and trailing qualifiers, so "TIMESTAMP(6) WITH LOCAL TIME ZONE" is
 * classed as TIMESTAMP - exactly today's strncmp(type, "TIMESTAMP", 9)
 * behaviour (proposal 0.9: time-zone columns are timestamps, zone-less).
 */

#ifndef DB_TYPE_CLASS_H
#define DB_TYPE_CLASS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DB_TYPE_CLASS_OTHER = 0,   /* anything not listed - never guessed */
    DB_TYPE_CLASS_STRING,
    DB_TYPE_CLASS_NUMERIC,
    DB_TYPE_CLASS_DATE,        /* date + time of day, no fraction      */
    DB_TYPE_CLASS_TIMESTAMP,   /* date + time + fractional seconds,
                                  including the WITH (LOCAL) TIME ZONE
                                  variants                              */
    DB_TYPE_CLASS_LOB
} db_type_class_t;

/* NULL or "" returns DB_TYPE_CLASS_OTHER. */
db_type_class_t db_type_class(const char *type_name);

/* "STRING", "NUMERIC", "DATE", "TIMESTAMP", "LOB" or "OTHER" - for logs. */
const char *db_type_class_name(db_type_class_t cls);

#ifdef __cplusplus
}
#endif

#endif /* DB_TYPE_CLASS_H */
