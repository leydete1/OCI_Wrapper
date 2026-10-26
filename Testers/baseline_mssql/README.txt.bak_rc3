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

Request_Test_Select_2.json  (point Z5)
  Same row (NUMBER_COL 4242) and the same values. Differences:
  - table definition: FIELD_TEST_ID (identity) exists on SQL Server only;
    ROWID_COL and UROWID_COL exist on Oracle only
  - BINARY_FLOAT_COL / BINARY_DOUBLE_COL (real / float): field_type NUMBER,
    plain text 2.5 / 1.4142135623000001 (Oracle: UNKNOWN, 2.5E+000 /
    1.4142135623000001E+000 - the same digits)
  - DATE_COL, TIMESTAMP_COL, TIMESTAMP_TZ_COL, TIMESTAMP_LTZ_COL:
    field_type TIMESTAMP with ISO text, e.g. 2030-01-02 08:30:00.123456
    (Oracle: DATE / TIMESTAMP / UNKNOWN with NLS text, 02-JAN-30 ...)
  - INTERVAL_YM_COL / INTERVAL_DS_COL: field_type STRING, the same text
    (stored as text on SQL Server; Oracle: UNKNOWN)
  RAW_COL is UNKNOWN / CAFEBABE on both.

Fixtures not run on SQL Server at all: see ../mssql_excluded.txt.
