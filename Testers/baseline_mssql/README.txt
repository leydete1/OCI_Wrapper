baseline_mssql - SQL Server baselines for Run_mssql.sh (driver_mssql Stage 7)

Most files here are the Oracle baselines from ../baseline, copied unchanged
by copy_baselines_mssql.py: those fixtures give the same response on both
databases.

The six fixtures below have a SQL Server baseline of their own. Each was
written by a clean Run_mssql.sh run (reset first) and reviewed against the
Oracle baseline on 23 Oct 2026. Each may differ from Oracle ONLY as stated
here; any other difference is a defect.

select_integration_tests.xml, select_integration_tests.json
  - alias case: Second_Blob / Third_Blob (SQL Server keeps the case as
    written; Oracle upper-cases unquoted aliases)
  - seed text: DESCRIPTION of ID 1 is "Adams pizza"
  - fetch_batch_size 5 (xml only)
  Same 5 rows, 15 BLOB files, same file sizes.

Unit_Test_Procedure_Round_2.xml, Unit_Test_Procedure_Round_2.json
  - DATE_COL: field_type TIMESTAMP, value 2030-01-01 00:00:00
    (datetime2; Oracle reports DATE, 01-JAN-30)
  Same 4 seed rows.

Verify_Round_5_Clob.xml
  - fetch_batch_size 5 (Oracle forces 1 when a CLOB column is selected)
  Same 2 rows, 501 then 502. The fixture has ORDER BY NUMBER_COL since
  23 Oct (point Z4, fixture MD5 2f3822848e8a3cebd5f08bb9792cc61b).

Request_Test_Select_2.json  (point Z5; updated 26 Oct for C3)
  Same row (NUMBER_COL 4242) and the same values. Since C3 (25 Oct) both
  databases give the same field_type for the float, interval, RAW and
  time-zone columns; what still differs is the text of the values and
  DATE_COL's type:
  - table definition: FIELD_TEST_ID (identity) exists on SQL Server only;
    ROWID_COL and UROWID_COL exist on Oracle only (STRING, empty)
  - BINARY_FLOAT_COL / BINARY_DOUBLE_COL (real / float): NUMBER on both;
    plain text 2.5 / 1.4142135623000001 (Oracle: 2.5E+000 /
    1.4142135623000001E+000 - the same digits)
  - DATE_COL: TIMESTAMP with ISO text, 2030-01-02 00:00:00 (Oracle: DATE,
    02-JAN-30)
  - TIMESTAMP_COL, TIMESTAMP_TZ_COL, TIMESTAMP_LTZ_COL: TIMESTAMP on both;
    ISO text, e.g. 2030-01-02 08:30:00.123456 (Oracle: NLS text,
    02-JAN-30 08.30.00.123456 AM ...)
  - INTERVAL_YM_COL / INTERVAL_DS_COL: STRING on both, the same text
    (stored as text on SQL Server)
  - RAW_COL: STRING / CAFEBABE on both
  Both baselines were promoted on 25 Oct after review (C3 changed only
  the field_type labels); backups *.bak_cu1.

Verify_Round_9_Blob.xml  (point C7, 26 Oct)
  Not one of the six above - it gives the same response as Oracle. The
  fixture gained ORDER BY NUMBER_COL (fixture MD5
  0e3b00bded54783cfa50fe85d204ad12): without it Oracle returned 902 before
  901 on 25 Oct. The response repeats the SQL in <sql_query>, so the
  baseline was promoted here and in ../baseline (backups *.bak_c7).

Fixtures not run on SQL Server at all: see ../mssql_excluded.txt.
