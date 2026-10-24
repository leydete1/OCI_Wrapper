baseline_mssql - SQL Server baselines for Run_mssql.sh (driver_mssql Stage 7)

Most files here are the Oracle baselines from ../baseline, copied unchanged
by copy_baselines_mssql.py: those fixtures give the same response on both
databases.

The fixtures below have a SQL Server baseline of their own, written by the
first Run_mssql.sh run and reviewed against the Oracle baseline. Each may
differ from Oracle ONLY as stated; any other difference is a defect.

  select_integration_tests.xml         alias case (Second_Blob vs SECOND_BLOB); seed DESCRIPTION text of row 1
  select_integration_tests.json        alias case (Second_Blob vs SECOND_BLOB); seed DESCRIPTION text of row 1
  Verify_Round_5_Clob.xml              fetch_batch_size 5 (Oracle forces 1 for a CLOB column)
  Unit_Test_Procedure_Round_2.xml      DATE_COL: field_type TIMESTAMP, value 2030-01-01 00:00:00 (Oracle DATE, 01-JAN-30)
  Unit_Test_Procedure_Round_2.json     DATE_COL: field_type TIMESTAMP, value 2030-01-01 00:00:00 (Oracle DATE, 01-JAN-30)
  Request_Test_Select_2.json           TO BE DETERMINED from its first SQL Server response (point Z5)

Fixtures not run on SQL Server at all: see ../mssql_excluded.txt.
