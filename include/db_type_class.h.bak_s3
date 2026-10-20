/*
 * db_type_class.h
 *
 * Oracle dialect extraction, Stage 1 (2026-09-30); extended in Stage 4b
 * (2026-10-06).
 *
 * Maps a column's type NAME, as the metadata layer reports it
 * (col_metadata_t.data_type, e.g. "VARCHAR2", "TIMESTAMP(6) WITH TIME
 * ZONE"), to a small vendor-neutral class. Core code compares classes,
 * never vendor type names - so a DATE check in Level 2 does not need
 * to know that Oracle spells its types differently from SQL Server.
 *
 * Today only Oracle's names are listed. driver_mssql adds its own
 * names here (DATETIME2, NVARCHAR, VARBINARY ...) when it arrives;
 * nothing that calls db_type_class() has to change for that.
 *
 * Matching is case-insensitive and ignores any "(n)" / "(p,s)" suffix
 * and trailing qualifiers, so "TIMESTAMP(6) WITH LOCAL TIME ZONE" is
 * classed as TIMESTAMP - exactly today's strncmp(type, "TIMESTAMP", 9)
 * behaviour (proposal 0.9: time-zone columns are timestamps, zone-less).
 * The one exception is INTERVAL, whose class depends on the qualifier:
 * "INTERVAL YEAR(2) TO MONTH" is INTERVAL_YM, "INTERVAL DAY(2) TO
 * SECOND(6)" is INTERVAL_DS.
 *
 * Stage 4b added INTERVAL_YM, INTERVAL_DS, BINARY and ROWID (so that
 * Insert_Validate_Module.c's validate_field() can dispatch on class),
 * moved ROWID/UROWID out of STRING into ROWID, added REAL and DOUBLE
 * PRECISION to NUMERIC, and added db_type_lob_kind().
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
    DB_TYPE_CLASS_LOB,
    /* ---- Stage 4b ---- */
    DB_TYPE_CLASS_INTERVAL_YM, /* year-month interval                  */
    DB_TYPE_CLASS_INTERVAL_DS, /* day-second interval                  */
    DB_TYPE_CLASS_BINARY,      /* short binary column, value as hex
                                  (Oracle RAW)                          */
    DB_TYPE_CLASS_ROWID        /* physical row address (Oracle ROWID,
                                  UROWID) - was STRING before 4b        */
} db_type_class_t;

/* NULL or "" returns DB_TYPE_CLASS_OTHER. */
db_type_class_t db_type_class(const char *type_name);

/* Class name for logs: "STRING", "NUMERIC", "DATE", "TIMESTAMP", "LOB",
 * "INTERVAL_YM", "INTERVAL_DS", "BINARY", "ROWID" or "OTHER". */
const char *db_type_class_name(db_type_class_t cls);

/*
 * db_type_lob_kind() - Stage 4b.
 *
 * Which LOB columns core writes after the statement, and how:
 *   DB_LOB_BINARY - written from a file (Oracle BLOB)
 *   DB_LOB_TEXT   - written from text   (Oracle CLOB, NCLOB)
 *   DB_LOB_NONE   - everything else, including Oracle BFILE: a BFILE
 *                   is a read-only pointer to an external file, so it
 *                   is not written this way (and is in the LOB class
 *                   but not here - the two answer different questions).
 *
 * Must agree with the driver's dialect lob_placeholder(): a column is
 * written after the statement exactly when the SQL gives it a LOB
 * placeholder instead of a bind. For Oracle both cover exactly BLOB,
 * CLOB and NCLOB.
 */
typedef enum {
    DB_LOB_NONE = 0,
    DB_LOB_BINARY,
    DB_LOB_TEXT
} db_lob_kind_t;

db_lob_kind_t db_type_lob_kind(const char *type_name);

#ifdef __cplusplus
}
#endif

#endif /* DB_TYPE_CLASS_H */
