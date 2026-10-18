-- ======================================================================
-- Reset_Tester_Objects_mssql.sql   (driver_mssql Stage 1, point S11)
--
-- SQL Server counterpart of Reset_Tester_Objects.sql, sections 3 and 4.
-- Puts the AUTHENTICATE test accounts and OCI_FIELD_TEST back into the
-- state the tester fixtures expect. Sections 1-2 of the Oracle script
-- (the FX_ objects of the DDL fixtures) have no counterpart: DDL stays
-- Oracle-only (M8, D10).
--
-- Run as DATA_MANAGER before every tester run against SQL Server:
--
--   sqlcmd -S localhost -U DATA_MANAGER -d DataManager -C -b \
--          -i Reset_Tester_Objects_mssql.sql
--
-- Safe to run any number of times. All of it runs in one transaction:
-- any error rolls everything back and stops the script.
-- ======================================================================

:on error exit
SET NOCOUNT ON;
SET XACT_ABORT ON;
GO
USE DataManager;
GO

BEGIN TRANSACTION;

DECLARE @n int;

-- ----------------------------------------------------------------------
-- 3. AUTHENTICATE test accounts (auth_max_failed_attempts = 5)
-- ----------------------------------------------------------------------
-- Round 4 trips the lock: one bad password away from it.
UPDATE DATA_MANAGER.APP_USER
   SET LOCKED = 'N', LOCKED_TS = NULL, FAILED_ATTEMPTS = 4
 WHERE USERNAME = 'auth.test.nearlock';
SET @n = @@ROWCOUNT;
PRINT 'ok      auth.test.nearlock  -> 4 failed attempts (' + CAST(@n AS varchar(10)) + ' row)';

-- Rounds 1/2/7 use this one; start clean.
UPDATE DATA_MANAGER.APP_USER
   SET LOCKED = 'N', LOCKED_TS = NULL, FAILED_ATTEMPTS = 0
 WHERE USERNAME = 'auth.test.user';
SET @n = @@ROWCOUNT;
PRINT 'ok      auth.test.user      -> clean (' + CAST(@n AS varchar(10)) + ' row)';

-- Round 5 needs an account that is already locked: a copy of
-- auth.test.user (same password hash and auth source), created once.
INSERT INTO DATA_MANAGER.APP_USER (USERNAME, DISPLAY_NAME, AUTH_SOURCE_ID, PASSWORD_HASH,
                                   PASSWORD_PARAMS, ENABLED, LOCKED, FAILED_ATTEMPTS, LOCKED_TS)
SELECT 'auth.test.locked', 'Auth Test Locked User', AUTH_SOURCE_ID, PASSWORD_HASH,
       PASSWORD_PARAMS, 'Y', 'Y', 5, SYSDATETIME()
  FROM DATA_MANAGER.APP_USER
 WHERE USERNAME = 'auth.test.user'
   AND NOT EXISTS (SELECT 1 FROM DATA_MANAGER.APP_USER
                    WHERE USERNAME = 'auth.test.locked');   -- _CI_ collation: case-blind

UPDATE DATA_MANAGER.APP_USER
   SET ENABLED = 'Y', LOCKED = 'Y', FAILED_ATTEMPTS = 5,
       LOCKED_TS = COALESCE(LOCKED_TS, SYSDATETIME())
 WHERE USERNAME = 'auth.test.locked';
SET @n = @@ROWCOUNT;
PRINT 'ok      auth.test.locked    -> locked, 5 failed attempts (' + CAST(@n AS varchar(10)) + ' row)';

-- ----------------------------------------------------------------------
-- 4. OCI_FIELD_TEST: empty it and reseed with fixed values
-- ----------------------------------------------------------------------
-- TRUNCATE (not DELETE) restarts FIELD_TEST_ID, so the seed rows are
-- 1-4 on every run. Values are the Oracle reset's; interval columns hold
-- Oracle's text form, dates are fixed (2030) as there.
TRUNCATE TABLE DATA_MANAGER.OCI_FIELD_TEST;

INSERT INTO DATA_MANAGER.OCI_FIELD_TEST (
    NUMBER_COL, NUMBER_P_S_COL, FLOAT_COL, BINARY_FLOAT_COL, BINARY_DOUBLE_COL,
    CHAR_COL, VARCHAR2_COL, NCHAR_COL, NVARCHAR2_COL,
    DATE_COL, TIMESTAMP_COL, TIMESTAMP_TZ_COL, TIMESTAMP_LTZ_COL,
    INTERVAL_YM_COL, INTERVAL_DS_COL, RAW_COL,
    CLOB_COL, NCLOB_COL, BLOB_COL)
VALUES (
    42, 12345.67, 3.14, 1.5, 2.718281828,
    'CHAR_TEST', 'VARCHAR2_TEST', N'NCHAR_TEST', N'NVARCHAR2_TEST',
    '2030-01-01 00:00:00',
    '2030-01-01 12:00:00.000000',
    '2030-01-01 12:00:00.000000 +00:00',
    '2030-01-01 12:00:00.000000',
    '+0002-06', '+0005 12:30:45.123456',
    0xDEADBEEF,
    'This is a CLOB test value.', N'This is an NCLOB test value.', NULL);

INSERT INTO DATA_MANAGER.OCI_FIELD_TEST (
    NUMBER_COL, NUMBER_P_S_COL, FLOAT_COL, BINARY_FLOAT_COL, BINARY_DOUBLE_COL,
    CHAR_COL, VARCHAR2_COL, NCHAR_COL, NVARCHAR2_COL,
    DATE_COL, TIMESTAMP_COL, TIMESTAMP_TZ_COL, TIMESTAMP_LTZ_COL,
    INTERVAL_YM_COL, INTERVAL_DS_COL, RAW_COL,
    CLOB_COL, NCLOB_COL, BLOB_COL)
VALUES (
    4242, 4242.42, 4.25, 2.5, 1.4142135623,
    'SEED_4242', 'Seed row 4242 - every type', N'SEED_4242', N'Seed row 4242 NVARCHAR2',
    '2030-01-02 00:00:00',
    '2030-01-02 08:30:00.123456',
    '2030-01-02 08:30:00.123456 +00:00',
    '2030-01-02 08:30:00.123456',
    '+0001-03', '+0002 03:04:05.000006',
    0xCAFEBABE,
    'Seed CLOB value for row 4242.', N'Seed NCLOB value for row 4242.',
    0x0102030405060708);

INSERT INTO DATA_MANAGER.OCI_FIELD_TEST (NUMBER_COL, VARCHAR2_COL, DATE_COL, CLOB_COL, BLOB_COL)
VALUES (4243, 'Seed BLOB row 4243', '2030-01-03 00:00:00',
        'Seed CLOB value for row 4243.', 0xDEADBEEF4243);

INSERT INTO DATA_MANAGER.OCI_FIELD_TEST (NUMBER_COL, VARCHAR2_COL, DATE_COL, CLOB_COL, BLOB_COL)
VALUES (4244, 'Seed BLOB row 4244', '2030-01-04 00:00:00',
        'Seed CLOB value for row 4244.', 0xDEADBEEF4244);

PRINT 'ok      OCI_FIELD_TEST      -> rows 42, 4242, 4243, 4244';

COMMIT;
GO

-- ----------------------------------------------------------------------
-- What the reset left behind (for the record)
-- ----------------------------------------------------------------------
SELECT USERNAME, LOCKED, FAILED_ATTEMPTS FROM DATA_MANAGER.APP_USER
 WHERE USERNAME LIKE 'auth.test.%' ORDER BY USERNAME;
SELECT FIELD_TEST_ID, NUMBER_COL, VARCHAR2_COL FROM DATA_MANAGER.OCI_FIELD_TEST ORDER BY NUMBER_COL;
GO
