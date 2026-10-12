-- ======================================================================
-- Reset_Tester_Objects.sql  (fix-22 points 5, 14 and 15, 2026-10-10)
--
-- Puts the database back into the state the HTTP tester fixtures expect.
-- Run it as DATA_MANAGER before every full tester run, after starting
-- nothing else that uses these objects:
--
--     sql DATA_MANAGER@localhost:1521/freepdb1 @Reset_Tester_Objects.sql
--
-- It is safe to run any number of times. Every DROP that finds nothing
-- prints "absent" and carries on; any other error stops the script.
--
-- What it does:
--   1. Drops every object a fixture CREATES, so the CREATE fixtures can
--      succeed every run (plus the old shared names, cleaned up once).
--   2. Recreates every object a fixture NEEDS to already exist: the
--      tables the DROP_TABLE fixtures drop, the table the
--      CREATE_PROCEDURE fixtures insert into, and the GRANT grantee.
--   3. Puts the AUTHENTICATE test accounts into their designed state.
--   4. Empties OCI_FIELD_TEST and reseeds it with fixed rows.
--
-- Privileges: DATA_MANAGER already creates users (the CREATE_USER
-- fixtures do). If DROP USER fails with ORA-01031, a DBA needs to run,
-- once:  GRANT DROP USER TO DATA_MANAGER;
-- ======================================================================

SET SERVEROUTPUT ON
WHENEVER SQLERROR EXIT SQL.SQLCODE ROLLBACK

DECLARE
    -- Runs one DDL statement. p_absent is the error that means "there
    -- was nothing to drop" (0 for a CREATE); that one is reported and
    -- skipped, anything else is raised and stops the script.
    PROCEDURE ddl(p_sql IN VARCHAR2, p_absent IN PLS_INTEGER DEFAULT 0) IS
    BEGIN
        EXECUTE IMMEDIATE p_sql;
        DBMS_OUTPUT.PUT_LINE('ok      ' || p_sql);
    EXCEPTION
        WHEN OTHERS THEN
            IF p_absent <> 0 AND SQLCODE = p_absent THEN
                DBMS_OUTPUT.PUT_LINE('absent  ' || p_sql);
            ELSE
                DBMS_OUTPUT.PUT_LINE('FAILED  ' || p_sql || '  -> ' || SQLERRM);
                RAISE;
            END IF;
    END;

    PROCEDURE drop_table(p_name IN VARCHAR2) IS
    BEGIN
        ddl('DROP TABLE DATA_MANAGER.' || p_name || ' CASCADE CONSTRAINTS PURGE', -942);
    END;
    PROCEDURE drop_view(p_name IN VARCHAR2) IS
    BEGIN
        ddl('DROP VIEW DATA_MANAGER.' || p_name, -942);
    END;
    PROCEDURE drop_proc(p_name IN VARCHAR2) IS
    BEGIN
        ddl('DROP PROCEDURE DATA_MANAGER.' || p_name, -4043);
    END;
    PROCEDURE drop_user(p_name IN VARCHAR2) IS
    BEGIN
        ddl('DROP USER ' || p_name || ' CASCADE', -1918);
    END;
BEGIN
    -- ---------------------------------------------------------------
    -- 1. Objects the fixtures create
    -- ---------------------------------------------------------------
    -- CreateTable / CreateUser / CreateView / CreateProcedure Round 1
    drop_table('FX_CT_LOG_X');   drop_table('FX_CT_LOG_J');
    drop_user ('FX_CU_USER_X');  drop_user ('FX_CU_USER_J');
    drop_view ('FX_CV_VIEW_X');  drop_view ('FX_CV_VIEW_J');
    drop_proc ('FX_CP_PROC_X');  drop_proc ('FX_CP_PROC_J');
    -- DDL_Sequence Round 1 (view and procedure first - they depend on
    -- the table)
    drop_view ('FX_SEQ_VIEW_X'); drop_view ('FX_SEQ_VIEW_J');
    drop_proc ('FX_SEQ_PROC_X'); drop_proc ('FX_SEQ_PROC_J');
    drop_table('FX_SEQ_LOG_X');  drop_table('FX_SEQ_LOG_J');
    drop_user ('FX_SEQ_USER_X'); drop_user ('FX_SEQ_USER_J');
    -- The names every fixture used to share (one-time clean-up; absent
    -- from then on)
    drop_view ('FIELD_TEST_SAMPLE');
    drop_proc ('LOG_MIGRATION_EVENT');
    drop_table('MIGRATION_LOG');
    drop_user ('MIGRATOR_USER');

    -- ---------------------------------------------------------------
    -- 2. Objects the fixtures need to exist
    -- ---------------------------------------------------------------
    -- DropTable Round 4 (plain confirmed drop) and Round 1 (PURGE)
    drop_table('FX_DROP_ME_X');
    ddl('CREATE TABLE DATA_MANAGER.FX_DROP_ME_X (ID NUMBER CONSTRAINT FX_DROP_ME_X_PK PRIMARY KEY)');
    drop_table('FX_DROP_ME_J');
    ddl('CREATE TABLE DATA_MANAGER.FX_DROP_ME_J (ID NUMBER CONSTRAINT FX_DROP_ME_J_PK PRIMARY KEY)');
    drop_table('FX_DROP_R1_X');
    ddl('CREATE TABLE DATA_MANAGER.FX_DROP_R1_X (ID NUMBER CONSTRAINT FX_DROP_R1_X_PK PRIMARY KEY)');
    drop_table('FX_DROP_R1_J');
    ddl('CREATE TABLE DATA_MANAGER.FX_DROP_R1_J (ID NUMBER CONSTRAINT FX_DROP_R1_J_PK PRIMARY KEY)');
    -- CreateProcedure Round 1 inserts into this, so it always compiles
    drop_table('FX_PROC_LOG');
    ddl('CREATE TABLE DATA_MANAGER.FX_PROC_LOG ('
        || 'LOG_ID NUMBER(10) CONSTRAINT FX_PROC_LOG_PK PRIMARY KEY, '
        || 'LOG_MESSAGE VARCHAR2(4000), '
        || 'CREATED_DATE DATE DEFAULT SYSDATE NOT NULL)');
    -- Grant Round 1 grants to this user
    drop_user('FX_GRANTEE');
    ddl('CREATE USER FX_GRANTEE IDENTIFIED BY "FxGrantee2026x"');
END;
/

-- ----------------------------------------------------------------------
-- 3. AUTHENTICATE test accounts (auth_max_failed_attempts = 5)
-- ----------------------------------------------------------------------
-- Round 4 trips the lock: one bad password away from it.
UPDATE APP_USER
   SET LOCKED = 'N', LOCKED_TS = NULL, FAILED_ATTEMPTS = 4
 WHERE USERNAME = 'auth.test.nearlock';

-- Rounds 1/2/7 use this one; start clean.
UPDATE APP_USER
   SET LOCKED = 'N', LOCKED_TS = NULL, FAILED_ATTEMPTS = 0
 WHERE USERNAME = 'auth.test.user';

-- Round 5 needs an account that is already locked. Created once as a
-- copy of auth.test.user (same password hash and auth source), then
-- forced into the locked state every run.
INSERT INTO APP_USER (USERNAME, DISPLAY_NAME, AUTH_SOURCE_ID, PASSWORD_HASH,
                      ENABLED, LOCKED, FAILED_ATTEMPTS, LOCKED_TS)
SELECT 'auth.test.locked', 'Auth Test Locked User', AUTH_SOURCE_ID, PASSWORD_HASH,
       'Y', 'Y', 5, SYSTIMESTAMP
  FROM APP_USER
 WHERE USERNAME = 'auth.test.user'
   AND NOT EXISTS (SELECT 1 FROM APP_USER
                    WHERE UPPER(USERNAME) = 'AUTH.TEST.LOCKED');

UPDATE APP_USER
   SET ENABLED = 'Y', LOCKED = 'Y', FAILED_ATTEMPTS = 5,
       LOCKED_TS = NVL(LOCKED_TS, SYSTIMESTAMP)
 WHERE USERNAME = 'auth.test.locked';

-- ----------------------------------------------------------------------
-- 4. OCI_FIELD_TEST: empty it and reseed with fixed values
-- ----------------------------------------------------------------------
-- Every row in it today is left over from earlier runs (about 1,780).
-- Dates are fixed (2030) rather than SYSDATE so results are the same on
-- every run, and so PURGE_OLD_ROWS (Procedure Round 4: DATE_COL older
-- than a year) never removes a seed row.
--
--   42        the original seed row (your create script), fixed dates
--   4242      every type filled, CLOB_COL and NCLOB_COL both set -
--             read by Request_Test_Select_2.json
--   4243/4244 small BLOB + CLOB - read by select_integration_tests
--
-- The INSERT/UPDATE/DELETE fixtures add and change their own rows
-- during the run as before; none of them touches 4242-4244.
DELETE FROM DATA_MANAGER.OCI_FIELD_TEST;

INSERT INTO DATA_MANAGER.OCI_FIELD_TEST (
    NUMBER_COL, NUMBER_P_S_COL, FLOAT_COL, BINARY_FLOAT_COL, BINARY_DOUBLE_COL,
    CHAR_COL, VARCHAR2_COL, NCHAR_COL, NVARCHAR2_COL,
    DATE_COL, TIMESTAMP_COL, TIMESTAMP_TZ_COL, TIMESTAMP_LTZ_COL,
    INTERVAL_YM_COL, INTERVAL_DS_COL, RAW_COL,
    CLOB_COL, NCLOB_COL, BLOB_COL)
VALUES (
    42, 12345.67, 3.14, 1.5f, 2.718281828,
    'CHAR_TEST', 'VARCHAR2_TEST', N'NCHAR_TEST', N'NVARCHAR2_TEST',
    DATE '2030-01-01',
    TIMESTAMP '2030-01-01 12:00:00.000000',
    TIMESTAMP '2030-01-01 12:00:00.000000 +00:00',
    TIMESTAMP '2030-01-01 12:00:00.000000',
    INTERVAL '2-6' YEAR TO MONTH, INTERVAL '5 12:30:45.123456' DAY TO SECOND,
    HEXTORAW('DEADBEEF'),
    'This is a CLOB test value.', N'This is an NCLOB test value.', NULL);

INSERT INTO DATA_MANAGER.OCI_FIELD_TEST (
    NUMBER_COL, NUMBER_P_S_COL, FLOAT_COL, BINARY_FLOAT_COL, BINARY_DOUBLE_COL,
    CHAR_COL, VARCHAR2_COL, NCHAR_COL, NVARCHAR2_COL,
    DATE_COL, TIMESTAMP_COL, TIMESTAMP_TZ_COL, TIMESTAMP_LTZ_COL,
    INTERVAL_YM_COL, INTERVAL_DS_COL, RAW_COL,
    CLOB_COL, NCLOB_COL, BLOB_COL)
VALUES (
    4242, 4242.42, 4.25, 2.5f, 1.4142135623,
    'SEED_4242', 'Seed row 4242 - every type', N'SEED_4242', N'Seed row 4242 NVARCHAR2',
    DATE '2030-01-02',
    TIMESTAMP '2030-01-02 08:30:00.123456',
    TIMESTAMP '2030-01-02 08:30:00.123456 +00:00',
    TIMESTAMP '2030-01-02 08:30:00.123456',
    INTERVAL '1-3' YEAR TO MONTH, INTERVAL '2 03:04:05.000006' DAY TO SECOND,
    HEXTORAW('CAFEBABE'),
    'Seed CLOB value for row 4242.', N'Seed NCLOB value for row 4242.',
    TO_BLOB(HEXTORAW('0102030405060708')));

INSERT INTO DATA_MANAGER.OCI_FIELD_TEST (NUMBER_COL, VARCHAR2_COL, DATE_COL, CLOB_COL, BLOB_COL)
VALUES (4243, 'Seed BLOB row 4243', DATE '2030-01-03',
        'Seed CLOB value for row 4243.', TO_BLOB(HEXTORAW('DEADBEEF4243')));

INSERT INTO DATA_MANAGER.OCI_FIELD_TEST (NUMBER_COL, VARCHAR2_COL, DATE_COL, CLOB_COL, BLOB_COL)
VALUES (4244, 'Seed BLOB row 4244', DATE '2030-01-04',
        'Seed CLOB value for row 4244.', TO_BLOB(HEXTORAW('DEADBEEF4244')));

COMMIT;

-- ----------------------------------------------------------------------
-- What the reset left behind (for the record)
-- ----------------------------------------------------------------------
SELECT USERNAME, LOCKED, FAILED_ATTEMPTS FROM APP_USER
 WHERE USERNAME LIKE 'auth.test.%' ORDER BY USERNAME;
SELECT NUMBER_COL, VARCHAR2_COL FROM DATA_MANAGER.OCI_FIELD_TEST ORDER BY NUMBER_COL;
