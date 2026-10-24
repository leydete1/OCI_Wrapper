-- ======================================================================
-- MSSQL_Diagnostics.sql   (SQL Server Operations & Troubleshooting Guide,
--                          Sections 4 and 5 - G3 verification, 24 Oct 2026;
--                          fix 1, 24 Oct: QUOTED_IDENTIFIER ON for the XML
--                          in section 8; section 3 shows whether each open
--                          transaction has written anything; section 7
--                          counts snapshot transactions;
--                          fix 2, 24 Oct: section 7 names the transactions
--                          holding row versions; section 8 reads the
--                          system_health files, not only its small
--                          in-memory buffer)
--
-- READ-ONLY. Looks at what DataSol_Gateway is doing on SQL Server:
-- its sessions, their open transactions, running requests and blocking,
-- the locks they hold, the tempdb version store, and the deadlocks SQL
-- Server's always-on system_health session has recorded. It changes
-- nothing: no KILL, no SET options kept, no data touched.
--
-- The gateway connects with APP=DataSol_Gateway (MSSQL_Pool.c), so its
-- sessions - and the SQL Server harnesses' - show program_name
-- 'DataSol_Gateway'.
--
-- The DMVs need VIEW SERVER STATE, which DATA_MANAGER does not have
-- (G2: run this as sa; the gateway login is not widened). The password is
-- typed at a prompt and handed to sqlcmd for this one command only:
--
--   cd ~/eclipse-workspace/OCI_Wrapper/MSSQL_Server/SQL
--   read -rs -p "sa password: " P; echo
--   SQLCMDPASSWORD="$P" sqlcmd -S localhost -U sa -C -W -s "|" \
--       -i MSSQL_Diagnostics.sql -o diag_out.txt; unset P
--
-- Best run while Testers/Run_mssql.sh is going (sessions and requests
-- show up), and once more with the gateway idle (open transactions left
-- behind - point Z7).
-- ======================================================================

SET NOCOUNT ON;
-- sqlcmd connects with QUOTED_IDENTIFIER OFF; XML methods (section 8)
-- need it ON (error 1934 otherwise). Holds for the rest of the session.
SET QUOTED_IDENTIFIER ON;
GO

PRINT '=== 1. Server, clock and DataManager settings';
SELECT CAST(SERVERPROPERTY('ProductVersion') AS varchar(20))  AS version,
       CAST(SERVERPROPERTY('ProductLevel')   AS varchar(20))  AS level,
       CAST(SERVERPROPERTY('Edition')        AS varchar(40))  AS edition,
       SYSDATETIMEOFFSET()                                    AS server_time;
SELECT name, collation_name,
       is_read_committed_snapshot_on,
       snapshot_isolation_state_desc
  FROM sys.databases
 WHERE name = N'DataManager';
GO

PRINT '=== 2. Gateway sessions (program_name DataSol_Gateway)';
SELECT s.session_id,
       s.login_name,
       s.host_name,
       s.status,
       s.open_transaction_count                                    AS open_tx,
       s.last_request_start_time,
       s.last_request_end_time,
       DATEDIFF(second, s.last_request_end_time, SYSDATETIME())    AS idle_s
  FROM sys.dm_exec_sessions s
 WHERE s.program_name = N'DataSol_Gateway'
 ORDER BY s.session_id;
GO

PRINT '=== 3. Open transactions on gateway sessions (Z7) - log_bytes 0 / wrote_at NULL = has written nothing';
SELECT s.session_id,
       at.transaction_id,
       at.name                                                     AS tx_name,
       at.transaction_begin_time,
       DATEDIFF(second, at.transaction_begin_time, SYSDATETIME())  AS age_s,
       CASE at.transaction_state WHEN 0 THEN 'not initialised'
                                 WHEN 1 THEN 'initialised, not started'
                                 WHEN 2 THEN 'active'
                                 WHEN 3 THEN 'ended (read-only)'
                                 WHEN 4 THEN 'commit started'
                                 WHEN 5 THEN 'prepared'
                                 WHEN 6 THEN 'committed'
                                 WHEN 7 THEN 'rolling back'
                                 WHEN 8 THEN 'rolled back'
                                 ELSE CAST(at.transaction_state AS varchar(4))
       END                                                         AS state,
       COALESCE(dt.database_transaction_log_bytes_used, 0)         AS log_bytes,
       dt.database_transaction_begin_time                          AS wrote_at
  FROM sys.dm_tran_session_transactions st
  JOIN sys.dm_tran_active_transactions  at ON at.transaction_id = st.transaction_id
  JOIN sys.dm_exec_sessions             s  ON s.session_id      = st.session_id
  LEFT JOIN sys.dm_tran_database_transactions dt
         ON dt.transaction_id = at.transaction_id
        AND dt.database_id    = DB_ID(N'DataManager')
 WHERE s.program_name = N'DataSol_Gateway'
 ORDER BY age_s DESC;
GO

PRINT '=== 4. Requests running now: gateway sessions, and anything blocked';
SELECT r.session_id,
       r.status,
       r.command,
       r.wait_type,
       r.wait_time                                                 AS wait_ms,
       r.blocking_session_id                                       AS blocked_by,
       DB_NAME(r.database_id)                                      AS db,
       LEFT(REPLACE(REPLACE(t.text, CHAR(13), ' '), CHAR(10), ' '), 200) AS sql_text
  FROM sys.dm_exec_requests r
  JOIN sys.dm_exec_sessions s ON s.session_id = r.session_id
 OUTER APPLY sys.dm_exec_sql_text(r.sql_handle) t
 WHERE r.session_id <> @@SPID
   AND (s.program_name = N'DataSol_Gateway' OR r.blocking_session_id <> 0)
 ORDER BY r.blocking_session_id DESC, r.session_id;
GO

PRINT '=== 5. Head blockers (block others, are not blocked) and their last SQL';
SELECT s.session_id,
       s.program_name,
       s.login_name,
       s.status,
       s.open_transaction_count                                    AS open_tx,
       LEFT(REPLACE(REPLACE(t.text, CHAR(13), ' '), CHAR(10), ' '), 200) AS last_sql
  FROM sys.dm_exec_sessions s
  LEFT JOIN sys.dm_exec_connections c ON c.session_id = s.session_id
 OUTER APPLY sys.dm_exec_sql_text(c.most_recent_sql_handle) t
 WHERE s.session_id IN (SELECT blocking_session_id
                          FROM sys.dm_exec_requests
                         WHERE blocking_session_id <> 0)
   AND s.session_id NOT IN (SELECT session_id
                              FROM sys.dm_exec_requests
                             WHERE blocking_session_id <> 0);
GO

PRINT '=== 6. Locks in DataManager, by session (beyond the shared DATABASE lock)';
SELECT l.request_session_id                                        AS session_id,
       l.resource_type,
       l.request_mode,
       l.request_status,
       COUNT(*)                                                    AS locks
  FROM sys.dm_tran_locks l
 WHERE l.resource_database_id = DB_ID(N'DataManager')
   AND NOT (l.resource_type = 'DATABASE' AND l.request_mode = 'S')
 GROUP BY l.request_session_id, l.resource_type, l.request_mode, l.request_status
 ORDER BY l.request_session_id, locks DESC;
GO

PRINT '=== 7. tempdb version store, and the transactions that use row versions';
SELECT SUM(version_store_reserved_page_count) * 8                 AS version_store_kb
  FROM tempdb.sys.dm_db_file_space_usage;
-- The transactions SQL Server counts as using row versions, and so can
-- hold version cleanup back (cleanup runs about once a minute). Compare
-- with section 3: are the gateway's open read transactions among them?
SELECT COUNT(*)                                                   AS active_snapshot_tx
  FROM sys.dm_tran_active_snapshot_database_transactions;
SELECT a.session_id,
       COALESCE(s.program_name, '')                               AS program_name,
       a.transaction_id,
       a.is_snapshot,
       a.elapsed_time_seconds                                     AS elapsed_s,
       COALESCE(s.open_transaction_count, 0)                      AS open_tx
  FROM sys.dm_tran_active_snapshot_database_transactions a
  LEFT JOIN sys.dm_exec_sessions s ON s.session_id = a.session_id
 ORDER BY a.elapsed_time_seconds DESC;
GO

PRINT '=== 8. Deadlocks recorded by system_health (its files in /var/opt/mssql/log; times are UTC)';
-- system_health runs all the time and writes system_health_*.xel files;
-- its in-memory ring buffer holds only the most recent events. The SQL
-- Server Transaction harness causes a deadlock on purpose (Test 2), so a
-- harness run leaves an entry here.
IF OBJECT_ID('tempdb..#dl') IS NOT NULL DROP TABLE #dl;
SELECT CAST(f.event_data AS xml)                                  AS x
  INTO #dl
  FROM sys.fn_xe_file_target_read_file(N'/var/opt/mssql/log/system_health*.xel', NULL, NULL, NULL) f
 WHERE f.object_name = N'xml_deadlock_report';
SELECT x.value('(event/@timestamp)[1]', 'datetime2(3)')           AS utc_time,
       x.value('(event/data/value/deadlock/victim-list/victimProcess/@id)[1]',
               'varchar(50)')                                     AS victim_process,
       x.value('(event/data/value/deadlock/process-list/process/@clientapp)[1]',
               'nvarchar(128)')                                   AS client_app
  FROM #dl
 ORDER BY utc_time DESC;
DROP TABLE #dl;
GO

PRINT '=== done (read-only - nothing was changed)';
GO
