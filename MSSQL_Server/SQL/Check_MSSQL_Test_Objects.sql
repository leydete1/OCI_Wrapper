-- ======================================================================
-- Check_MSSQL_Test_Objects.sql   (driver_mssql Stage 1 proof)
--
-- Run as DATA_MANAGER after Create_MSSQL_Test_Objects.sql:
--
--   sqlcmd -S localhost -U DATA_MANAGER -d DataManager -C \
--          -i Check_MSSQL_Test_Objects.sql
--
-- Prints PASS or FAIL per check and a total. Changes nothing except
-- running PURGE_OLD_ROWS, which removes no seed row (seed dates are 2030).
-- ======================================================================

SET NOCOUNT ON;
GO
USE DataManager;
GO

IF OBJECT_ID('tempdb..#r') IS NOT NULL DROP TABLE #r;
CREATE TABLE #r (n int IDENTITY, ok bit, what nvarchar(200), got nvarchar(200));
GO

-- ---- Database settings (S2) and identity (S3) -------------------------
INSERT #r (ok, what, got)
SELECT CASE WHEN collation_name = 'Latin1_General_100_CI_AS_SC_UTF8' THEN 1 ELSE 0 END,
       'collation is UTF-8 (D4)', collation_name
  FROM sys.databases WHERE name = 'DataManager';
INSERT #r (ok, what, got)
SELECT is_read_committed_snapshot_on, 'READ_COMMITTED_SNAPSHOT ON (D3)',
       CAST(is_read_committed_snapshot_on AS nvarchar(5))
  FROM sys.databases WHERE name = 'DataManager';
INSERT #r (ok, what, got)
SELECT CASE WHEN SCHEMA_NAME() = 'DATA_MANAGER' AND USER_NAME() = 'DATA_MANAGER' THEN 1 ELSE 0 END,
       'connected as DATA_MANAGER, default schema DATA_MANAGER', USER_NAME() + ' / ' + SCHEMA_NAME();
INSERT #r (ok, what, got)
SELECT CASE WHEN USER_NAME(s.principal_id) = 'dbo' THEN 1 ELSE 0 END,
       'schema DATA_MANAGER owned by dbo', USER_NAME(s.principal_id)
  FROM sys.schemas s WHERE s.name = 'DATA_MANAGER';
GO

-- ---- Row counts (S7, S8, S10, S11) -------------------------------------
DECLARE @t TABLE (tbl sysname, want int);
INSERT @t VALUES ('AUTH_SOURCE', 2), ('APP_USER', 5), ('ROLE', 1), ('PERMISSION', 2),
                 ('USER_ROLE', 1), ('ROLE_PERMISSION', 1), ('OCI_FIELD_TEST', 4),
                 ('OCI_LOB_TEST', 6), ('UNIT_TEST_FIELD_TEST', 0), ('OCI_CLOB_TEST', 0),
                 ('OCI_SESSION', 0), ('OCI_METRICS', 0), ('AUDIT_TRAIL', 0);
DECLARE @tbl sysname, @want int, @got int, @sql nvarchar(400);
DECLARE c CURSOR LOCAL FAST_FORWARD FOR SELECT tbl, want FROM @t;
OPEN c; FETCH c INTO @tbl, @want;
WHILE @@FETCH_STATUS = 0
BEGIN
    SET @sql = N'SELECT @g = COUNT(*) FROM DATA_MANAGER.' + QUOTENAME(@tbl);
    EXEC sp_executesql @sql, N'@g int OUTPUT', @g = @got OUTPUT;
    INSERT #r (ok, what, got)
    VALUES (CASE WHEN @got = @want THEN 1 ELSE 0 END,
            @tbl + ' has ' + CAST(@want AS varchar(10)) + ' rows', CAST(@got AS varchar(10)));
    FETCH c INTO @tbl, @want;
END
CLOSE c; DEALLOCATE c;
GO

-- ---- Photos: byte sizes of the originals (S8) --------------------------
INSERT #r (ok, what, got)
SELECT CASE WHEN DATALENGTH(l.PHOTO) = v.bytes THEN 1 ELSE 0 END,
       'photo ' + v.f + ' is ' + CAST(v.bytes AS varchar(10)) + ' bytes',
       CAST(DATALENGTH(l.PHOTO) AS varchar(20))
  FROM (VALUES (1, 'Adam_1.jpg', 456547), (2, 'Emily_big_hug.jpg', 72585),
               (3, 'Emily_Football.jpg', 423891), (4, 'Sophi.jpg', 502790),
               (5, 'The_Gang.jpg', 670535), (6, 'Uncle_James.jpg', 26263)) AS v (id, f, bytes)
  LEFT JOIN DATA_MANAGER.OCI_LOB_TEST l ON l.ID = v.id AND l.FILE_NAME = v.f;
GO

-- ---- Seed LOB values, byte for byte (S11) ------------------------------
INSERT #r (ok, what, got)
SELECT CASE WHEN f.BLOB_COL = v.b AND f.CLOB_COL = v.c THEN 1 ELSE 0 END,
       'seed row ' + CAST(v.n AS varchar(10)) + ' CLOB and BLOB',
       CONVERT(varchar(40), f.BLOB_COL, 1)
  FROM (VALUES (4242, 0x0102030405060708, 'Seed CLOB value for row 4242.'),
               (4243, 0xDEADBEEF4243,     'Seed CLOB value for row 4243.'),
               (4244, 0xDEADBEEF4244,     'Seed CLOB value for row 4244.')) AS v (n, b, c)
  LEFT JOIN DATA_MANAGER.OCI_FIELD_TEST f ON f.NUMBER_COL = v.n;
INSERT #r (ok, what, got)
SELECT CASE WHEN NCLOB_COL = N'Seed NCLOB value for row 4242.' AND RAW_COL = 0xCAFEBABE
             AND INTERVAL_DS_COL = '+0002 03:04:05.000006' THEN 1 ELSE 0 END,
       'seed row 4242 NCLOB, RAW and interval', INTERVAL_DS_COL
  FROM DATA_MANAGER.OCI_FIELD_TEST WHERE NUMBER_COL = 4242;
INSERT #r (ok, what, got)
SELECT CASE WHEN MIN(FIELD_TEST_ID) = 1 AND MAX(FIELD_TEST_ID) = 4 THEN 1 ELSE 0 END,
       'seed rows have FIELD_TEST_ID 1-4',
       CAST(MIN(FIELD_TEST_ID) AS varchar(10)) + '-' + CAST(MAX(FIELD_TEST_ID) AS varchar(10))
  FROM DATA_MANAGER.OCI_FIELD_TEST;
GO

-- ---- Privileges (S3, S6) -----------------------------------------------
INSERT #r (ok, what, got)
SELECT CASE WHEN HAS_PERMS_BY_NAME('DATA_MANAGER.AUDIT_TRAIL', 'OBJECT', 'INSERT') = 1
             AND HAS_PERMS_BY_NAME('DATA_MANAGER.AUDIT_TRAIL', 'OBJECT', 'SELECT') = 1 THEN 1 ELSE 0 END,
       'AUDIT_TRAIL: INSERT and SELECT allowed', NULL;

DECLARE @e int;
SET @e = 0;
BEGIN TRY
    EXEC (N'UPDATE DATA_MANAGER.AUDIT_TRAIL SET CHANGE_REASON = CHANGE_REASON WHERE 1 = 0');
END TRY
BEGIN CATCH SET @e = ERROR_NUMBER(); END CATCH;
INSERT #r (ok, what, got) VALUES (CASE WHEN @e = 229 THEN 1 ELSE 0 END,
       'AUDIT_TRAIL: UPDATE denied (error 229)', CAST(@e AS varchar(10)));

SET @e = 0;
BEGIN TRY
    EXEC (N'DELETE FROM DATA_MANAGER.AUDIT_TRAIL WHERE 1 = 0');
END TRY
BEGIN CATCH SET @e = ERROR_NUMBER(); END CATCH;
INSERT #r (ok, what, got) VALUES (CASE WHEN @e = 229 THEN 1 ELSE 0 END,
       'AUDIT_TRAIL: DELETE denied (error 229)', CAST(@e AS varchar(10)));

SET @e = 0;
BEGIN TRY
    EXEC (N'CREATE TABLE DATA_MANAGER.DM_PROBE (ID int)');
    EXEC (N'DROP TABLE DATA_MANAGER.DM_PROBE');
END TRY
BEGIN CATCH SET @e = ERROR_NUMBER(); END CATCH;
INSERT #r (ok, what, got) VALUES (CASE WHEN @e = 262 THEN 1 ELSE 0 END,
       'CREATE TABLE refused (error 262)', CAST(@e AS varchar(10)));
GO

-- ---- Procedures (S9) ----------------------------------------------------
DECLARE @i int, @b bigint, @cnt int;
DECLARE @rs TABLE (NUMBER_COL bigint, VARCHAR2_COL varchar(100), DATE_COL datetime2(0));
DECLARE @rs1 TABLE (NUMBER_COL bigint);

EXEC DATA_MANAGER.GET_DEPT_STATUS @P_DEPT_ID = 4242, @P_STATUS = @i OUTPUT;
INSERT #r (ok, what, got) VALUES (CASE WHEN @i = 1 THEN 1 ELSE 0 END,
       'GET_DEPT_STATUS(4242) -> P_STATUS 1', CAST(@i AS varchar(10)));

INSERT @rs EXEC DATA_MANAGER.GET_FIELD_ROWS @P_MIN_NUM = 4243;
SELECT @cnt = COUNT(*) FROM @rs;
INSERT #r (ok, what, got) VALUES (CASE WHEN @cnt = 2 THEN 1 ELSE 0 END,
       'GET_FIELD_ROWS(4243) -> 2 rows', CAST(@cnt AS varchar(10)));

DELETE FROM @rs;
INSERT @rs EXEC DATA_MANAGER.GET_ROWS_WITH_STATUS @P_MIN_NUM = 4242, @P_ROW_COUNT = @i OUTPUT;
SELECT @cnt = COUNT(*) FROM @rs;
INSERT #r (ok, what, got) VALUES (CASE WHEN @i = 3 AND @cnt = 3 THEN 1 ELSE 0 END,
       'GET_ROWS_WITH_STATUS(4242) -> P_ROW_COUNT 3, 3 rows',
       CAST(@i AS varchar(10)) + ' / ' + CAST(@cnt AS varchar(10)));

-- @@TRANCOUNT is read into a variable first: inside the INSERT below it
-- would always be 1 (the INSERT's own autocommit transaction).
DECLARE @tc int;
EXEC DATA_MANAGER.PURGE_OLD_ROWS;
SET @tc = @@TRANCOUNT;
SELECT @cnt = COUNT(*) FROM DATA_MANAGER.OCI_FIELD_TEST;
INSERT #r (ok, what, got) VALUES (CASE WHEN @cnt = 4 AND @tc = 0 THEN 1 ELSE 0 END,
       'PURGE_OLD_ROWS runs, keeps the 4 seed rows, leaves no transaction open',
       CAST(@cnt AS varchar(10)) + ' rows, trancount ' + CAST(@tc AS varchar(10)));

DELETE FROM @rs;
INSERT @rs EXEC DATA_MANAGER.UNIT_TEST_SAMPLE_PROC @P_IN_NUM = 21, @P_OUT_NUM = @b OUTPUT;
INSERT #r (ok, what, got) VALUES (CASE WHEN @b = 42 THEN 1 ELSE 0 END,
       'UNIT_TEST_SAMPLE_PROC(21) -> P_OUT_NUM 42', CAST(@b AS varchar(20)));

EXEC DATA_MANAGER.UNIT_TEST_NOOP_PROC;
INSERT #r (ok, what, got) VALUES (1, 'UNIT_TEST_NOOP_PROC runs', NULL);

INSERT @rs1 EXEC DATA_MANAGER.UNIT_TEST_CURSOR_PROC @P_OPEN_CURSOR = 0;
SELECT @cnt = COUNT(*) FROM @rs1;
INSERT #r (ok, what, got) VALUES (CASE WHEN @cnt = 0 THEN 1 ELSE 0 END,
       'UNIT_TEST_CURSOR_PROC(0) -> no result set', CAST(@cnt AS varchar(10)));

SET @b = 41;
EXEC DATA_MANAGER.UNIT_TEST_INOUT_PROC @P_VALUE = @b OUTPUT;
INSERT #r (ok, what, got) VALUES (CASE WHEN @b = 42 THEN 1 ELSE 0 END,
       'UNIT_TEST_INOUT_PROC(41) -> 42', CAST(@b AS varchar(20)));
GO

-- ---- Report --------------------------------------------------------------
SELECT CASE ok WHEN 1 THEN 'PASS' ELSE 'FAIL' END AS result, what AS [check], got
  FROM #r ORDER BY n;
SELECT SUM(CAST(ok AS int)) AS passed, COUNT(*) - SUM(CAST(ok AS int)) AS failed, COUNT(*) AS total
  FROM #r;
GO
