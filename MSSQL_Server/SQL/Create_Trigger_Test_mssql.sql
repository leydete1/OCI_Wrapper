-- ======================================================================
-- Create_Trigger_Test_mssql.sql   (clean-up Part 2, C5-P / C5-T, 26 Oct 2026)
--
-- Test tables with AFTER INSERT/UPDATE/DELETE triggers, for the C5 probe
-- (MSSQL_C5_Probe) and, later, the C5 harness tests:
--
--   DATA_MANAGER.MS_TRIGGER_TEST     trigger WITHOUT SET NOCOUNT ON - each
--                                    firing sends its own row counts to
--                                    the client (the case C5 is about)
--   DATA_MANAGER.MS_TRIGGER_NC_TEST  the same trigger WITH SET NOCOUNT ON
--   DATA_MANAGER.MS_TRIGGER_LOG      what both triggers write
--
-- Each trigger, for a statement that touches N rows, runs two statements:
--   1. INSERT into MS_TRIGGER_LOG three rows per row touched  -> count 3N
--   2. an UPDATE of MS_TRIGGER_LOG that matches nothing        -> count 0
-- so the statement's own count (N) can never be mistaken for the
-- trigger's (3N, 0). A trigger fires even when N = 0.
--
-- Run as sa (creating tables and triggers needs more than DATA_MANAGER
-- has; DATA_MANAGER already has SELECT/INSERT/UPDATE/DELETE on the
-- schema). sqlcmd prompts for the password:
--
--   sqlcmd -S localhost -U sa -C -b -d DataManager \
--          -i Create_Trigger_Test_mssql.sql
--
-- Safe to run any number of times: tables are created only if missing,
-- triggers with CREATE OR ALTER. Nothing in this file is a secret.
-- ======================================================================

:on error exit
SET NOCOUNT ON;
GO
USE DataManager;
GO

IF OBJECT_ID(N'DATA_MANAGER.MS_TRIGGER_TEST', N'U') IS NULL
    CREATE TABLE DATA_MANAGER.MS_TRIGGER_TEST (
        ID     int            NOT NULL CONSTRAINT PK_MS_TRIGGER_TEST PRIMARY KEY,
        NAME   nvarchar(100),
        DOC    varbinary(max),
        NOTE   nvarchar(max)
    );
GO
IF OBJECT_ID(N'DATA_MANAGER.MS_TRIGGER_NC_TEST', N'U') IS NULL
    CREATE TABLE DATA_MANAGER.MS_TRIGGER_NC_TEST (
        ID     int            NOT NULL CONSTRAINT PK_MS_TRIGGER_NC_TEST PRIMARY KEY,
        NAME   nvarchar(100),
        DOC    varbinary(max),
        NOTE   nvarchar(max)
    );
GO
IF OBJECT_ID(N'DATA_MANAGER.MS_TRIGGER_LOG', N'U') IS NULL
    CREATE TABLE DATA_MANAGER.MS_TRIGGER_LOG (
        LOG_ID     bigint IDENTITY(1,1) NOT NULL CONSTRAINT PK_MS_TRIGGER_LOG PRIMARY KEY,
        TABLE_NAME varchar(30)   NOT NULL,
        ID         int           NOT NULL,
        ACTION     char(1)       NOT NULL,
        COPY_NO    int           NOT NULL,
        LOGGED_TS  datetime2(6)  NOT NULL CONSTRAINT DF_MS_TRIGGER_LOG_TS DEFAULT SYSDATETIME()
    );
GO

-- Deliberately NO "SET NOCOUNT ON": its two statements report their
-- row counts to the client.
CREATE OR ALTER TRIGGER DATA_MANAGER.TR_MS_TRIGGER_TEST
ON DATA_MANAGER.MS_TRIGGER_TEST
AFTER INSERT, UPDATE, DELETE
AS
BEGIN
    DECLARE @act char(1) =
        CASE WHEN EXISTS (SELECT 1 FROM inserted) AND EXISTS (SELECT 1 FROM deleted) THEN 'U'
             WHEN EXISTS (SELECT 1 FROM inserted) THEN 'I'
             ELSE 'D' END;

    INSERT INTO DATA_MANAGER.MS_TRIGGER_LOG (TABLE_NAME, ID, ACTION, COPY_NO)
    SELECT 'MS_TRIGGER_TEST', x.ID, @act, c.n
    FROM (SELECT ID FROM inserted
          UNION ALL
          SELECT ID FROM deleted WHERE NOT EXISTS (SELECT 1 FROM inserted)) AS x
    CROSS JOIN (VALUES (1), (2), (3)) AS c(n);

    UPDATE DATA_MANAGER.MS_TRIGGER_LOG SET COPY_NO = COPY_NO WHERE 1 = 0;
END;
GO

CREATE OR ALTER TRIGGER DATA_MANAGER.TR_MS_TRIGGER_NC_TEST
ON DATA_MANAGER.MS_TRIGGER_NC_TEST
AFTER INSERT, UPDATE, DELETE
AS
BEGIN
    SET NOCOUNT ON;
    DECLARE @act char(1) =
        CASE WHEN EXISTS (SELECT 1 FROM inserted) AND EXISTS (SELECT 1 FROM deleted) THEN 'U'
             WHEN EXISTS (SELECT 1 FROM inserted) THEN 'I'
             ELSE 'D' END;

    INSERT INTO DATA_MANAGER.MS_TRIGGER_LOG (TABLE_NAME, ID, ACTION, COPY_NO)
    SELECT 'MS_TRIGGER_NC_TEST', x.ID, @act, c.n
    FROM (SELECT ID FROM inserted
          UNION ALL
          SELECT ID FROM deleted WHERE NOT EXISTS (SELECT 1 FROM inserted)) AS x
    CROSS JOIN (VALUES (1), (2), (3)) AS c(n);

    UPDATE DATA_MANAGER.MS_TRIGGER_LOG SET COPY_NO = COPY_NO WHERE 1 = 0;
END;
GO

PRINT 'ok      MS_TRIGGER_TEST, MS_TRIGGER_NC_TEST, MS_TRIGGER_LOG and their triggers';
SELECT t.name AS table_name, tr.name AS trigger_name, tr.is_disabled
FROM sys.triggers tr JOIN sys.tables t ON t.object_id = tr.parent_id
WHERE t.name IN (N'MS_TRIGGER_TEST', N'MS_TRIGGER_NC_TEST')
ORDER BY t.name;
GO
