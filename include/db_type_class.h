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
 * Oracle's names and, since driver_mssql Stage 3 (2026-10-19), SQL
 * Server's (NVARCHAR, DATETIME2, VARBINARY ...) are listed; nothing
 * that calls db_type_class() had to change for that. SQL Server's
 * classes: char, varchar, nchar, nvarchar, uniqueidentifier and time
 * STRING; int, bigint, smallint, tinyint, bit, decimal, numeric,
 * money, smallmoney, float and real NUMERIC; date and smalldatetime
 * DATE; datetime, datetime2 and datetimeoffset TIMESTAMP; binary(n)
 * and varbinary(n) BINARY; any "(MAX)" type, text, ntext, image and
 * xml LOB.
 *
 * Matching is case-insensitive and ignores any "(n)" / "(p,s)" suffix
 * (but not SQL Server's "(MAX)", which makes the column LOB)
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
 * CLOB and NCLOB; for SQL Server VARBINARY(MAX) and IMAGE (binary)
 * only. SQL Server's text LOBs - VARCHAR(MAX), NVARCHAR(MAX), TEXT,
 * NTEXT and XML - are LOB class but DB_LOB_NONE: they are bound in
 * the statement itself (driver_mssql Stage 4, U6), see
 * db_type_text_bound() below.
 */
typedef enum {
    DB_LOB_NONE = 0,
    DB_LOB_BINARY,
    DB_LOB_TEXT
} db_lob_kind_t;

db_lob_kind_t db_type_lob_kind(const char *type_name);

/*
 * db_type_text_bound() - driver_mssql Stage 4 (2026-10-20, U6).
 *
 * 1 for a text LOB the driver binds in the INSERT/UPDATE statement
 * itself rather than writing it afterwards: SQL Server's VARCHAR(MAX),
 * NVARCHAR(MAX), TEXT, NTEXT and XML. Core then binds the whole value
 * (a file:// value read from its file - lob_text_bind.h) and counts
 * it as a LOB written. 0 for everything else, every Oracle type
 * included - Oracle CLOBs are written after the statement as before.
 * A SQL Server AUDIT_TRAIL row therefore never needs an UPDATE.
 */
int db_type_text_bound(const char *type_name);

#ifdef __cplusplus
}
#endif

#endif /* DB_TYPE_CLASS_H */
