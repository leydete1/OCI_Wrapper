/*
 * db_metadata.h
 *
 * Vendor-neutral table metadata types (Oracle dialect extraction,
 * Stage 4a, 2026-10-06).
 *
 * col_metadata_t and metadata_request_t used to live in
 * Table_Metadata_Module.h next to the Oracle code that filled them.
 * They are now produced by the driver (db_driver_t.describe_table) and
 * consumed by core (metadata_cache.c, Level 2, the INSERT/UPDATE/DELETE
 * modules), so they need a home that belongs to neither side.
 *
 * Deliberately has NO includes. db_driver.h, Table_Metadata_Module.h
 * and metadata_cache_meta.h all include it, and Connection.h already
 * includes metadata_cache.h - an include-free header cannot create a
 * cycle between them (see metadata_cache.h's own header comment for the
 * cycle this avoids).
 *
 * The struct layouts are unchanged from Table_Metadata_Module.h, byte
 * for byte. metadata_cache.c stores col_metadata_t arrays with a raw
 * memcpy, so a layout change would also be a cache format change.
 *
 * data_type still holds the vendor's own type name (Oracle: "VARCHAR2",
 * "NUMBER", "TIMESTAMP(6)" ...). Core code that branches on those names
 * is converted to type classes in Stage 4b.
 */

#ifndef DB_METADATA_H
#define DB_METADATA_H

/* Maximum columns supported per table / result set */
#define MAX_TABLE_COLUMNS  1024

/* ------------------------------------------------------------------ */
/*  col_metadata_t - one entry per table column.                       */
/* ------------------------------------------------------------------ */
typedef struct {
    char  col_name    [128];  /* column name                           */
    char  data_type   [128];  /* vendor type name, e.g. "VARCHAR2"     */
    int   data_length;        /* length in bytes / chars               */
    int   data_precision;     /* precision      (-1 = not applicable)  */
    int   data_scale;         /* scale          (-1 = not applicable)  */
    char  nullable    [4];    /* "Y" or "N"                            */
    char  data_default[512];  /* column default (empty string if none) */
    char  source_table[128];  /* not set by describe_table - always "" */
} col_metadata_t;

/* ------------------------------------------------------------------ */
/*  metadata_request_t - input to describe_table / get_request_metadata */
/* ------------------------------------------------------------------ */
typedef struct {
    char  table_name[128];    /* target table; upper-cased in place    */
    char  owner     [128];    /* schema owner; upper-cased in place.   */
                              /* If empty, the driver resolves it and  */
                              /* writes it back here.                  */
} metadata_request_t;

#endif /* DB_METADATA_H */
