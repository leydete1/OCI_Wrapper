-- ======================================================================
-- Create_MSSQL_Test_Objects.sql   (driver_mssql Stage 1, points S1-S10)
--
-- Builds the SQL Server test database for driver_mssql: database
-- DataManager, login/user DATA_MANAGER, schema DATA_MANAGER (owned by
-- dbo), the gateway tables, the test tables, the test procedures, the
-- six photos and the OCI_FIELD_TEST seed rows.
--
-- Run ONCE, as sa, from the folder holding this file and
-- MSSQL_Security_Seed.sql (the security rows exported from Oracle):
--
--   docker exec mssql2022 mkdir -p /var/opt/mssql/photos
--   docker cp /opt/oracle/Photos/. mssql2022:/var/opt/mssql/photos/
--   read -s -p "DATA_MANAGER password: " DM_PW; echo
--   sqlcmd -S localhost -U sa -C -b -i Create_MSSQL_Test_Objects.sql \
--          -v DM_PASSWORD="$DM_PW" RECREATE=NO
--   unset DM_PW
--
-- sqlcmd prompts for the sa password. Nothing in this file is a secret.
--
-- RECREATE=NO   stops at once if DataManager already exists.
-- RECREATE=YES  drops DataManager (and everything in it) and rebuilds it.
-- The DATA_MANAGER password must not contain a single quote.
-- ======================================================================

:on error exit
SET NOCOUNT ON;
GO

-- ----------------------------------------------------------------------
-- S1. Guard
-- ----------------------------------------------------------------------
IF DB_ID(N'DataManager') IS NOT NULL AND N'$(RECREATE)' <> N'YES'
    THROW 50000, N'DataManager already exists - nothing changed. Re-run with -v RECREATE=YES to drop it and build it again.', 1;
GO

IF DB_ID(N'DataManager') IS NOT NULL
BEGIN
    ALTER DATABASE DataManager SET SINGLE_USER WITH ROLLBACK IMMEDIATE;
    DROP DATABASE DataManager;
    PRINT 'ok      dropped DataManager (RECREATE=YES)';
END
GO

-- ----------------------------------------------------------------------
-- S2. Database: UTF-8 collation (D4), read committed snapshot (D3)
-- ----------------------------------------------------------------------
CREATE DATABASE DataManager COLLATE Latin1_General_100_CI_AS_SC_UTF8;
GO
ALTER DATABASE DataManager SET READ_COMMITTED_SNAPSHOT ON;
GO
PRINT 'ok      database DataManager';
GO

-- ----------------------------------------------------------------------
-- S3. Login, user, schema, privileges
-- ----------------------------------------------------------------------
IF SUSER_ID(N'DATA_MANAGER') IS NULL
    CREATE LOGIN DATA_MANAGER WITH PASSWORD = N'$(DM_PASSWORD)',
        DEFAULT_DATABASE = DataManager, CHECK_POLICY = ON;
ELSE
    ALTER LOGIN DATA_MANAGER WITH PASSWORD = N'$(DM_PASSWORD)',
        DEFAULT_DATABASE = DataManager;
GO
PRINT 'ok      login DATA_MANAGER';
GO

USE DataManager;
GO
CREATE SCHEMA DATA_MANAGER AUTHORIZATION dbo;
GO
CREATE USER DATA_MANAGER FOR LOGIN DATA_MANAGER WITH DEFAULT_SCHEMA = DATA_MANAGER;
GO
-- DML and EXECUTE only. No CREATE / ALTER rights (DDL is out of scope on
-- SQL Server, M8). The schema is owned by dbo, so DENY works (S6).
GRANT SELECT, INSERT, UPDATE, DELETE, EXECUTE ON SCHEMA::DATA_MANAGER TO DATA_MANAGER;
-- PREREQUISITE (driver_mssql Stage 3, V1, 2026-10-20): read-only right
-- to see definitions in this schema. Without it SQL Server hides every
-- default constraint's text from DATA_MANAGER, describe_table reports
-- no defaults, and Level 2 refuses INSERTs that leave a NOT NULL
-- column with a default (APP_USER.ENABLED ...) to that default. On
-- Oracle DATA_MANAGER owns the tables and sees them anyway.
GRANT VIEW DEFINITION ON SCHEMA::DATA_MANAGER TO DATA_MANAGER;
GO
PRINT 'ok      user DATA_MANAGER, schema DATA_MANAGER (owner dbo), VIEW DEFINITION';
GO

-- ----------------------------------------------------------------------
-- S5. Gateway tables (security, session, metrics, audit)
-- ----------------------------------------------------------------------
CREATE TABLE DATA_MANAGER.AUTH_SOURCE (
    AUTH_SOURCE_ID   bigint IDENTITY(1,1) NOT NULL CONSTRAINT PK_AUTH_SOURCE PRIMARY KEY,
    NAME             varchar(100)  NOT NULL,
    SOURCE_TYPE      varchar(20)   NOT NULL
                        CONSTRAINT CK_AUTH_SOURCE_TYPE CHECK (SOURCE_TYPE IN ('LOCAL', 'LDAP', 'AD')),
    CONFIGURATION    varchar(max),
    ENABLED          varchar(1)    NOT NULL CONSTRAINT DF_AUTH_SOURCE_ENABLED DEFAULT 'Y'
                        CONSTRAINT CK_AUTH_SOURCE_ENABLED CHECK (ENABLED IN ('Y', 'N')),
    CREATED_TS       datetime2(6)  NOT NULL CONSTRAINT DF_AUTH_SOURCE_CREATED  DEFAULT SYSDATETIME(),
    MODIFIED_TS      datetime2(6)  NOT NULL CONSTRAINT DF_AUTH_SOURCE_MODIFIED DEFAULT SYSDATETIME()
);
CREATE UNIQUE INDEX IDX_AUTH_SOURCE_NAME_UQ ON DATA_MANAGER.AUTH_SOURCE (NAME);

CREATE TABLE DATA_MANAGER.APP_USER (
    USER_ID          bigint IDENTITY(1,1) NOT NULL CONSTRAINT PK_APP_USER PRIMARY KEY,
    USERNAME         varchar(128)  NOT NULL,
    DISPLAY_NAME     varchar(255),
    AUTH_SOURCE_ID   bigint        NOT NULL
                        CONSTRAINT FK_APP_USER_AUTH_SOURCE REFERENCES DATA_MANAGER.AUTH_SOURCE (AUTH_SOURCE_ID),
    PASSWORD_HASH    varchar(255),
    PASSWORD_PARAMS  varchar(255),
    ENABLED          varchar(1)    NOT NULL CONSTRAINT DF_APP_USER_ENABLED DEFAULT 'Y'
                        CONSTRAINT CK_APP_USER_ENABLED CHECK (ENABLED IN ('Y', 'N')),
    LOCKED           varchar(1)    NOT NULL CONSTRAINT DF_APP_USER_LOCKED DEFAULT 'N'
                        CONSTRAINT CK_APP_USER_LOCKED CHECK (LOCKED IN ('Y', 'N')),
    FAILED_ATTEMPTS  bigint        NOT NULL CONSTRAINT DF_APP_USER_FAILED DEFAULT 0,
    LOCKED_TS        datetime2(6),
    LAST_LOGIN_TS    datetime2(6),
    CREATED_TS       datetime2(6)  NOT NULL CONSTRAINT DF_APP_USER_CREATED  DEFAULT SYSDATETIME(),
    MODIFIED_TS      datetime2(6)  NOT NULL CONSTRAINT DF_APP_USER_MODIFIED DEFAULT SYSDATETIME()
);
-- Oracle: unique on UPPER(USERNAME). The _CI_ collation already makes a
-- plain unique index case-blind.
CREATE UNIQUE INDEX IDX_APP_USER_USERNAME_UQ ON DATA_MANAGER.APP_USER (USERNAME);
CREATE INDEX IDX_APP_USER_AUTH_SOURCE ON DATA_MANAGER.APP_USER (AUTH_SOURCE_ID);

CREATE TABLE DATA_MANAGER.ROLE (
    ROLE_ID          bigint IDENTITY(1,1) NOT NULL CONSTRAINT PK_ROLE PRIMARY KEY,
    ROLE_NAME        varchar(100)  NOT NULL,
    DESCRIPTION      varchar(500)
);
CREATE UNIQUE INDEX IDX_ROLE_NAME_UQ ON DATA_MANAGER.ROLE (ROLE_NAME);

CREATE TABLE DATA_MANAGER.PERMISSION (
    PERMISSION_ID    bigint IDENTITY(1,1) NOT NULL CONSTRAINT PK_PERMISSION PRIMARY KEY,
    PERMISSION_CODE  varchar(100)  NOT NULL,
    DESCRIPTION      varchar(500)
);
CREATE UNIQUE INDEX IDX_PERMISSION_CODE_UQ ON DATA_MANAGER.PERMISSION (PERMISSION_CODE);

CREATE TABLE DATA_MANAGER.USER_ROLE (
    USER_ID          bigint NOT NULL CONSTRAINT FK_USER_ROLE_USER REFERENCES DATA_MANAGER.APP_USER (USER_ID),
    ROLE_ID          bigint NOT NULL CONSTRAINT FK_USER_ROLE_ROLE REFERENCES DATA_MANAGER.ROLE (ROLE_ID),
    CONSTRAINT PK_USER_ROLE PRIMARY KEY (USER_ID, ROLE_ID)
);

CREATE TABLE DATA_MANAGER.ROLE_PERMISSION (
    ROLE_ID          bigint NOT NULL CONSTRAINT FK_ROLE_PERM_ROLE REFERENCES DATA_MANAGER.ROLE (ROLE_ID),
    PERMISSION_ID    bigint NOT NULL CONSTRAINT FK_ROLE_PERM_PERM REFERENCES DATA_MANAGER.PERMISSION (PERMISSION_ID),
    CONSTRAINT PK_ROLE_PERMISSION PRIMARY KEY (ROLE_ID, PERMISSION_ID)
);
GO

CREATE TABLE DATA_MANAGER.OCI_SESSION (
    SESSION_ROW_ID   bigint IDENTITY(1,1) NOT NULL CONSTRAINT PK_OCI_SESSION PRIMARY KEY,
    SESSION_ID       varchar(36)   NOT NULL,
    STATUS           varchar(20)   NOT NULL
                        CONSTRAINT CK_OCI_SESSION_STATUS CHECK (STATUS IN ('CREATED', 'ACTIVE', 'EXPIRED',
                                                                           'LOGGED_OUT', 'EXPIRED_ORPHAN')),
    TTL_SECONDS      bigint        NOT NULL,
    CLIENT_ID        varchar(128),
    CLIENT_IP        varchar(45),
    CLIENT_HOST      varchar(128),
    APPLICATION_NAME varchar(100),
    TRANSACTION_ID   varchar(100),
    CREATED_TS       datetime2(6)  NOT NULL CONSTRAINT DF_OCI_SESSION_CREATED  DEFAULT SYSDATETIME(),
    LAST_ACTIVITY_TS datetime2(6)  NOT NULL CONSTRAINT DF_OCI_SESSION_ACTIVITY DEFAULT SYSDATETIME(),
    CLOSED_TS        datetime2(6),
    CLOSE_REASON     varchar(255)
);
CREATE UNIQUE INDEX IDX_SESSION_ID_UQ          ON DATA_MANAGER.OCI_SESSION (SESSION_ID);
CREATE INDEX        IDX_SESSION_STATUS_CREATED ON DATA_MANAGER.OCI_SESSION (STATUS, CREATED_TS);
CREATE INDEX        IDX_SESSION_TRANSACTION    ON DATA_MANAGER.OCI_SESSION (TRANSACTION_ID);
CREATE INDEX        IDX_SESSION_CLIENT         ON DATA_MANAGER.OCI_SESSION (CLIENT_ID, CREATED_TS);
GO

CREATE TABLE DATA_MANAGER.OCI_METRICS (
    METRICS_ROW_ID        bigint IDENTITY(1,1) NOT NULL CONSTRAINT PK_OCI_METRICS PRIMARY KEY,
    CONSUMER_NAME         varchar(64),
    SESSION_ID            varchar(64),
    TRANSACTION_ID        varchar(64),
    TRANSACTION_NAME      varchar(128),
    AUDIT_ID              varchar(64),
    CLIENT_IP             varchar(64),
    HOST_NAME             varchar(128),
    SERVER_IP             varchar(64),
    SERVER_PORT           bigint,
    PROCESS_ID            bigint,
    THREAD_ID             bigint,
    DATASOURCE_NAME       varchar(128),
    CONNECTION_ID         bigint,
    POOL_ID               bigint,
    OPERATION             varchar(32),
    OBJECT_NAME           varchar(128),
    SQL_HASH              bigint,
    CACHE_KEY_HASH        bigint,
    START_TIME_TS         datetime2(6),
    END_TIME_TS           datetime2(6),
    CACHE_LOOKUP_US       bigint,
    LEVEL1_PARSE_US       bigint,
    LEVEL2_PARSE_US       bigint,
    SQL_PARSE_US          bigint,
    EXECUTION_US          bigint,
    TOTAL_US              bigint,
    ROWS_AFFECTED         bigint,
    OUTPUT_XML_BYTES      bigint,
    CLOB_BYTES            bigint,
    LOB_BYTES             bigint,
    BYTES_PROCESSED       bigint,
    CACHE_HIT             decimal(1,0),
    STATUS_CODE           bigint,
    ERROR_CODE            varchar(64),
    ERROR_TEXT            varchar(255),
    CONNECTION_WAIT_US    bigint,
    CONNECTION_CREATE_US  bigint,
    CONNECTION_ACQUIRE_US bigint,
    INSERTED_TS           datetime2(6) NOT NULL CONSTRAINT DF_OCI_METRICS_INSERTED DEFAULT SYSDATETIME()
);
CREATE INDEX IDX_METRICS_START_TIME     ON DATA_MANAGER.OCI_METRICS (START_TIME_TS);
CREATE INDEX IDX_METRICS_OPERATION_TIME ON DATA_MANAGER.OCI_METRICS (OPERATION, START_TIME_TS);
CREATE INDEX IDX_METRICS_CONSUMER_TIME  ON DATA_MANAGER.OCI_METRICS (CONSUMER_NAME, START_TIME_TS);
GO

CREATE TABLE DATA_MANAGER.AUDIT_TRAIL (
    AUDIT_ID            bigint IDENTITY(1,1) NOT NULL CONSTRAINT PK_AUDIT_TRAIL PRIMARY KEY,
    TABLE_NAME          varchar(128)  NOT NULL,
    RECORD_ID           varchar(255)  NOT NULL,
    FIELD_NAME          varchar(128)  NOT NULL,
    ACTION_TYPE         varchar(10)   NOT NULL
                           CONSTRAINT CK_AUDIT_ACTION CHECK (ACTION_TYPE IN ('INSERT', 'UPDATE', 'DELETE')),
    OLD_VALUE           varchar(max),
    NEW_VALUE           varchar(max),
    DATA_TYPE           varchar(50),
    CHANGED_BY          varchar(100)  NOT NULL,
    CHANGED_BY_DB_USER  varchar(100)  NOT NULL CONSTRAINT DF_AUDIT_DB_USER DEFAULT SUSER_SNAME(),
    CHANGE_TIMESTAMP    datetime2(6)  NOT NULL CONSTRAINT DF_AUDIT_TS DEFAULT SYSDATETIME(),
    CHANGE_REASON       varchar(500)  NOT NULL,
    TRANSACTION_ID      varchar(100)  NOT NULL,
    SESSION_ID          varchar(100),
    CLIENT_IP           varchar(45),
    APPLICATION_NAME    varchar(100),
    MODULE_NAME         varchar(100),
    ESIG_ID             bigint,
    ESIG_MEANING        varchar(100),
    ESIG_TIMESTAMP      datetime2(6),
    ESIG_REQUIRED       varchar(1)    NOT NULL CONSTRAINT DF_AUDIT_ESIG_REQ DEFAULT 'N'
                           CONSTRAINT CK_AUDIT_ESIG_REQ CHECK (ESIG_REQUIRED IN ('Y', 'N')),
    ROW_HASH            varchar(64)   NOT NULL
);
CREATE INDEX IDX_AT_TABLE_RECORD ON DATA_MANAGER.AUDIT_TRAIL (TABLE_NAME, RECORD_ID);
CREATE INDEX IDX_AT_TIMESTAMP    ON DATA_MANAGER.AUDIT_TRAIL (CHANGE_TIMESTAMP);
CREATE INDEX IDX_AT_CHANGED_BY   ON DATA_MANAGER.AUDIT_TRAIL (CHANGED_BY, CHANGE_TIMESTAMP);
CREATE INDEX IDX_AT_TRANSACTION  ON DATA_MANAGER.AUDIT_TRAIL (TRANSACTION_ID);
CREATE INDEX IDX_AT_ESIG         ON DATA_MANAGER.AUDIT_TRAIL (ESIG_ID);
GO

-- S6. Write-once by permission, not by trigger: an enabled trigger would
-- make every INSERT ... OUTPUT on this table fail (error 334), and the
-- audit CLOB inserts use the OUTPUT row locator.
DENY UPDATE, DELETE ON DATA_MANAGER.AUDIT_TRAIL TO DATA_MANAGER;
GO
PRINT 'ok      gateway tables (9), AUDIT_TRAIL write-once for DATA_MANAGER';
GO

-- ----------------------------------------------------------------------
-- S7. Test tables
-- ----------------------------------------------------------------------
CREATE TABLE DATA_MANAGER.OCI_FIELD_TEST (
    FIELD_TEST_ID      bigint IDENTITY(1,1) NOT NULL CONSTRAINT PK_OCI_FIELD_TEST PRIMARY KEY,  -- 5c
    NUMBER_COL         bigint,
    NUMBER_P_S_COL     decimal(10,2),
    FLOAT_COL          float,
    BINARY_FLOAT_COL   real,
    BINARY_DOUBLE_COL  float,
    CHAR_COL           char(10),
    VARCHAR2_COL       varchar(100),
    NCHAR_COL          nchar(10),
    NVARCHAR2_COL      nvarchar(100),
    DATE_COL           datetime2(0),
    TIMESTAMP_COL      datetime2(6),
    TIMESTAMP_TZ_COL   datetimeoffset(6),
    TIMESTAMP_LTZ_COL  datetime2(6),
    INTERVAL_YM_COL    varchar(30),        -- Oracle's text form, e.g. +0002-06
    INTERVAL_DS_COL    varchar(30),        -- e.g. +0005 12:30:45.123456
    RAW_COL            varbinary(50),
    CLOB_COL           varchar(max),
    NCLOB_COL          nvarchar(max),
    BLOB_COL           varbinary(max)
);
-- The reset empties it with TRUNCATE, so the seed rows get the same
-- FIELD_TEST_ID (1-4) on every run. TRUNCATE needs ALTER on the table:
-- granted on this one test table only.
GRANT ALTER ON DATA_MANAGER.OCI_FIELD_TEST TO DATA_MANAGER;

CREATE TABLE DATA_MANAGER.UNIT_TEST_FIELD_TEST (
    NUMBER_COL    bigint        NOT NULL CONSTRAINT UNIT_TEST_FIELD_TEST_PK PRIMARY KEY,
    VARCHAR2_COL  varchar(100),
    DATE_COL      datetime2(0),
    CLOB_COL      varchar(max)
);
-- No UNIT_TEST_FIELD_TEST_DEL_BLOCK trigger (S7): Unit_Test_Module is
-- out of scope (M8), and a trigger would break INSERT ... OUTPUT.

CREATE TABLE DATA_MANAGER.OCI_LOB_TEST (
    ID           bigint        NOT NULL CONSTRAINT PK_OCI_LOB_TEST PRIMARY KEY,
    DESCRIPTION  varchar(200),
    FILE_NAME    varchar(100),
    PHOTO        varbinary(max)
);

CREATE TABLE DATA_MANAGER.OCI_CLOB_TEST (
    ID           bigint        NOT NULL CONSTRAINT PK_OCI_CLOB_TEST PRIMARY KEY,
    DESCRIPTION  varchar(200),
    FILE_NAME    varchar(100),
    LARGE_CLOB   varchar(max)
);
GO
PRINT 'ok      test tables (4)';
GO

-- ----------------------------------------------------------------------
-- S8. Photos (copied into the container first - see the header)
-- ----------------------------------------------------------------------
INSERT INTO DATA_MANAGER.OCI_LOB_TEST (ID, DESCRIPTION, FILE_NAME, PHOTO)
SELECT 1, 'Adams pizza', 'Adam_1.jpg', b.BulkColumn
  FROM OPENROWSET(BULK N'/var/opt/mssql/photos/Adam_1.jpg', SINGLE_BLOB) AS b;
INSERT INTO DATA_MANAGER.OCI_LOB_TEST (ID, DESCRIPTION, FILE_NAME, PHOTO)
SELECT 2, 'Big hug for Emily', 'Emily_big_hug.jpg', b.BulkColumn
  FROM OPENROWSET(BULK N'/var/opt/mssql/photos/Emily_big_hug.jpg', SINGLE_BLOB) AS b;
INSERT INTO DATA_MANAGER.OCI_LOB_TEST (ID, DESCRIPTION, FILE_NAME, PHOTO)
SELECT 3, 'Football Team', 'Emily_Football.jpg', b.BulkColumn
  FROM OPENROWSET(BULK N'/var/opt/mssql/photos/Emily_Football.jpg', SINGLE_BLOB) AS b;
INSERT INTO DATA_MANAGER.OCI_LOB_TEST (ID, DESCRIPTION, FILE_NAME, PHOTO)
SELECT 4, 'Cousin Sophie', 'Sophi.jpg', b.BulkColumn
  FROM OPENROWSET(BULK N'/var/opt/mssql/photos/Sophi.jpg', SINGLE_BLOB) AS b;
INSERT INTO DATA_MANAGER.OCI_LOB_TEST (ID, DESCRIPTION, FILE_NAME, PHOTO)
SELECT 5, 'The kids', 'The_Gang.jpg', b.BulkColumn
  FROM OPENROWSET(BULK N'/var/opt/mssql/photos/The_Gang.jpg', SINGLE_BLOB) AS b;
INSERT INTO DATA_MANAGER.OCI_LOB_TEST (ID, DESCRIPTION, FILE_NAME, PHOTO)
SELECT 6, 'The one and only James', 'Uncle_James.jpg', b.BulkColumn
  FROM OPENROWSET(BULK N'/var/opt/mssql/photos/Uncle_James.jpg', SINGLE_BLOB) AS b;
GO
PRINT 'ok      OCI_LOB_TEST: 6 photos';
GO

-- ----------------------------------------------------------------------
-- S9. Procedures (T-SQL). SET NOCOUNT ON in each, so row-count messages
-- never get in the way of result sets and OUT values.
-- ----------------------------------------------------------------------
-- Round 1: scalar IN + scalar OUT
CREATE PROCEDURE DATA_MANAGER.GET_DEPT_STATUS
    @P_DEPT_ID  bigint,
    @P_STATUS   int OUTPUT
AS
BEGIN
    SET NOCOUNT ON;
    SELECT @P_STATUS = COUNT(*)
      FROM DATA_MANAGER.OCI_FIELD_TEST
     WHERE NUMBER_COL = @P_DEPT_ID;
END;
GO

-- Round 2: IN + result set (CURSOR OUT on Oracle; 6c)
CREATE PROCEDURE DATA_MANAGER.GET_FIELD_ROWS
    @P_MIN_NUM  bigint
AS
BEGIN
    SET NOCOUNT ON;
    SELECT NUMBER_COL, VARCHAR2_COL, DATE_COL
      FROM DATA_MANAGER.OCI_FIELD_TEST
     WHERE NUMBER_COL >= @P_MIN_NUM
     ORDER BY NUMBER_COL;
END;
GO

-- Round 3: IN + scalar OUT + result set
CREATE PROCEDURE DATA_MANAGER.GET_ROWS_WITH_STATUS
    @P_MIN_NUM    bigint,
    @P_ROW_COUNT  int OUTPUT
AS
BEGIN
    SET NOCOUNT ON;
    SELECT @P_ROW_COUNT = COUNT(*)
      FROM DATA_MANAGER.OCI_FIELD_TEST
     WHERE NUMBER_COL >= @P_MIN_NUM;

    SELECT NUMBER_COL, VARCHAR2_COL, DATE_COL
      FROM DATA_MANAGER.OCI_FIELD_TEST
     WHERE NUMBER_COL >= @P_MIN_NUM
     ORDER BY NUMBER_COL;
END;
GO

-- Round 4: no parameters; commits its own work, as the Oracle one does.
-- (Called inside a transaction the caller opened explicitly, the COMMIT
-- would end that transaction too and SQL Server reports error 266. The
-- gateway opens no transaction around a procedure call.)
CREATE PROCEDURE DATA_MANAGER.PURGE_OLD_ROWS
AS
BEGIN
    SET NOCOUNT ON;
    DELETE FROM DATA_MANAGER.OCI_FIELD_TEST
     WHERE DATE_COL < DATEADD(day, -365, SYSDATETIME());
    IF @@TRANCOUNT > 0 COMMIT;
END;
GO

CREATE PROCEDURE DATA_MANAGER.UNIT_TEST_SAMPLE_PROC
    @P_IN_NUM   bigint,
    @P_OUT_NUM  bigint OUTPUT
AS
BEGIN
    SET NOCOUNT ON;
    SET @P_OUT_NUM = @P_IN_NUM * 2;
    SELECT NUMBER_COL, VARCHAR2_COL, DATE_COL
      FROM DATA_MANAGER.UNIT_TEST_FIELD_TEST
     WHERE NUMBER_COL = @P_IN_NUM;
END;
GO

CREATE PROCEDURE DATA_MANAGER.UNIT_TEST_NOOP_PROC
AS
BEGIN
    SET NOCOUNT ON;
    RETURN;
END;
GO

-- Returns a result set only when @P_OPEN_CURSOR = 1 (Oracle: the cursor
-- stays unopened otherwise).
CREATE PROCEDURE DATA_MANAGER.UNIT_TEST_CURSOR_PROC
    @P_OPEN_CURSOR  int
AS
BEGIN
    SET NOCOUNT ON;
    IF @P_OPEN_CURSOR = 1
        SELECT NUMBER_COL FROM DATA_MANAGER.UNIT_TEST_FIELD_TEST
         WHERE NUMBER_COL = 1;
END;
GO

-- New (S9): a real IN OUT parameter - returns its input plus one.
CREATE PROCEDURE DATA_MANAGER.UNIT_TEST_INOUT_PROC
    @P_VALUE  bigint OUTPUT
AS
BEGIN
    SET NOCOUNT ON;
    SET @P_VALUE = @P_VALUE + 1;
END;
GO
PRINT 'ok      procedures (8)';
GO

-- ----------------------------------------------------------------------
-- S10. Security rows exported from Oracle (IDs kept, so the foreign
-- keys match). Generated file - stays on the laptop, not in the project.
-- ----------------------------------------------------------------------
:r MSSQL_Security_Seed.sql
GO

-- OCI_FIELD_TEST seed rows: the same as the reset script.
:r Reset_Tester_Objects_mssql.sql
GO

PRINT 'ok      DataManager is ready';
GO
