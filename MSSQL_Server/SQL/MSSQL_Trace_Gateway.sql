-- ======================================================================
-- MSSQL_Trace_Gateway.sql   (SQL Server Operations & Troubleshooting
--                            Guide, Section 6 - G3 verification, 24 Oct)
--
-- An Extended Events trace of DataSol_Gateway's own statements and
-- errors - the SQL Server counterpart of the Oracle guide's
-- DBMS_MONITOR.SERV_MOD_ACT_TRACE_ENABLE + tkprof (Oracle guide, 8.3-8.6).
--
-- Not read-only in one respect: START creates a server-level event
-- session, DataSol_Gateway_Trace, and STOP / DROP stop and remove it.
-- No table or data is touched. The trace files go to SQL Server's log
-- folder in the container (/var/opt/mssql/log/DataSol_Gateway_Trace*.xel).
--
-- What is captured, for client_app_name 'DataSol_Gateway' only:
--   sql_batch_completed, rpc_completed   every statement, with duration
--                                         and row count (the ODBC driver
--                                         sends parameterised statements
--                                         as RPCs)
--   error_reported (severity >= 11)      every error, with its number
--                                         (1205, 1222, 266, 2627 ...)
--   plus xml_deadlock_report             (server-wide - a deadlock is
--                                         raised by the server, not the
--                                         client)
--
-- Run as sa (VIEW SERVER STATE and ALTER ANY EVENT SESSION - G2), with
-- the action as a sqlcmd variable:
--
--   cd ~/eclipse-workspace/OCI_Wrapper/MSSQL_Server/SQL
--   read -rs -p "sa password: " P; echo
--   SQLCMDPASSWORD="$P" sqlcmd -S localhost -U sa -C -W -s "|" \
--       -i MSSQL_Trace_Gateway.sql -v ACTION=START
--   ... run Testers/Run_mssql.sh ...
--   SQLCMDPASSWORD="$P" sqlcmd -S localhost -U sa -C -W -s "|" \
--       -i MSSQL_Trace_Gateway.sql -v ACTION=STOP
--   SQLCMDPASSWORD="$P" sqlcmd -S localhost -U sa -C -W -s "|" \
--       -i MSSQL_Trace_Gateway.sql -v ACTION=READ -o trace_out.txt
--   SQLCMDPASSWORD="$P" sqlcmd -S localhost -U sa -C -W -s "|" \
--       -i MSSQL_Trace_Gateway.sql -v ACTION=DROP
--   unset P
--
-- ACTION: START (creates the session if needed and starts it), STOP,
-- READ (summarises the files; works whether the session runs or not),
-- DROP (stops and removes the session; the .xel files stay - remove them
-- with: docker exec mssql2022 sh -c 'rm /var/opt/mssql/log/DataSol_Gateway_Trace*.xel').
-- ======================================================================

SET NOCOUNT ON;
GO

IF N'$(ACTION)' NOT IN (N'START', N'STOP', N'READ', N'DROP')
    THROW 50000, N'ACTION must be START, STOP, READ or DROP (sqlcmd -v ACTION=...)', 1;
GO

-- ---- START -------------------------------------------------------------
IF N'$(ACTION)' = N'START'
BEGIN
    IF NOT EXISTS (SELECT 1 FROM sys.server_event_sessions WHERE name = N'DataSol_Gateway_Trace')
        EXEC (N'
CREATE EVENT SESSION DataSol_Gateway_Trace ON SERVER
ADD EVENT sqlserver.sql_batch_completed (
    ACTION (sqlserver.session_id, sqlserver.client_app_name)
    WHERE  (sqlserver.client_app_name = N''DataSol_Gateway'')),
ADD EVENT sqlserver.rpc_completed (
    ACTION (sqlserver.session_id, sqlserver.client_app_name)
    WHERE  (sqlserver.client_app_name = N''DataSol_Gateway'')),
ADD EVENT sqlserver.error_reported (
    ACTION (sqlserver.session_id, sqlserver.client_app_name, sqlserver.sql_text)
    WHERE  (severity >= 11 AND sqlserver.client_app_name = N''DataSol_Gateway'')),
ADD EVENT sqlserver.xml_deadlock_report
ADD TARGET package0.event_file (SET filename = N''DataSol_Gateway_Trace'', max_file_size = 50, max_rollover_files = 4)
WITH (MAX_DISPATCH_LATENCY = 5 SECONDS, STARTUP_STATE = OFF);');

    IF NOT EXISTS (SELECT 1 FROM sys.dm_xe_sessions WHERE name = N'DataSol_Gateway_Trace')
        ALTER EVENT SESSION DataSol_Gateway_Trace ON SERVER STATE = START;

    PRINT 'ok      DataSol_Gateway_Trace started';
END
GO

-- ---- STOP / DROP -------------------------------------------------------
IF N'$(ACTION)' IN (N'STOP', N'DROP')
   AND EXISTS (SELECT 1 FROM sys.dm_xe_sessions WHERE name = N'DataSol_Gateway_Trace')
BEGIN
    ALTER EVENT SESSION DataSol_Gateway_Trace ON SERVER STATE = STOP;
    PRINT 'ok      DataSol_Gateway_Trace stopped';
END
GO

IF N'$(ACTION)' = N'DROP'
   AND EXISTS (SELECT 1 FROM sys.server_event_sessions WHERE name = N'DataSol_Gateway_Trace')
BEGIN
    DROP EVENT SESSION DataSol_Gateway_Trace ON SERVER;
    PRINT 'ok      DataSol_Gateway_Trace removed (the .xel files are kept)';
END
GO

-- ---- READ --------------------------------------------------------------
IF N'$(ACTION)' = N'READ'
BEGIN
    IF OBJECT_ID('tempdb..#ev') IS NOT NULL DROP TABLE #ev;

    SELECT f.object_name                                              AS event_name,
           CAST(f.event_data AS xml)                                  AS x
      INTO #ev
      FROM sys.fn_xe_file_target_read_file(N'/var/opt/mssql/log/DataSol_Gateway_Trace*.xel', NULL, NULL, NULL) f;

    PRINT '=== events by type';
    SELECT event_name, COUNT(*) AS events
      FROM #ev GROUP BY event_name ORDER BY events DESC;

    PRINT '=== errors (number, severity, message) - most frequent first';
    SELECT x.value('(event/data[@name="error_number"]/value)[1]', 'int')            AS error_number,
           x.value('(event/data[@name="severity"]/value)[1]', 'int')                AS severity,
           LEFT(x.value('(event/data[@name="message"]/value)[1]', 'nvarchar(400)'), 150) AS message,
           COUNT(*)                                                                  AS times
      FROM #ev WHERE event_name = 'error_reported'
     GROUP BY x.value('(event/data[@name="error_number"]/value)[1]', 'int'),
              x.value('(event/data[@name="severity"]/value)[1]', 'int'),
              LEFT(x.value('(event/data[@name="message"]/value)[1]', 'nvarchar(400)'), 150)
     ORDER BY times DESC;

    PRINT '=== 20 slowest statements (duration in ms; event time UTC)';
    SELECT TOP (20)
           x.value('(event/@timestamp)[1]', 'datetime2(3)')                         AS utc_time,
           x.value('(event/action[@name="session_id"]/value)[1]', 'int')            AS session_id,
           x.value('(event/data[@name="duration"]/value)[1]', 'bigint') / 1000     AS duration_ms,
           x.value('(event/data[@name="row_count"]/value)[1]', 'bigint')           AS row_count,
           LEFT(REPLACE(REPLACE(COALESCE(
                x.value('(event/data[@name="batch_text"]/value)[1]', 'nvarchar(max)'),
                x.value('(event/data[@name="statement"]/value)[1]', 'nvarchar(max)')),
                CHAR(13), ' '), CHAR(10), ' '), 200)                                AS sql_text
      FROM #ev
     WHERE event_name IN ('sql_batch_completed', 'rpc_completed')
     ORDER BY duration_ms DESC;

    PRINT '=== deadlocks (UTC)';
    SELECT x.value('(event/@timestamp)[1]', 'datetime2(3)')                         AS utc_time
      FROM #ev WHERE event_name = 'xml_deadlock_report'
     ORDER BY utc_time;

    DROP TABLE #ev;
END
GO
