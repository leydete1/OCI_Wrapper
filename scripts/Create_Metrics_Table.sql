
CREATE TABLE OCI_METRICS (
    -- Primary Key
    METRICS_ROW_ID            NUMBER          GENERATED ALWAYS AS IDENTITY PRIMARY KEY,

    -- Consumer Identity (closure item 5, 2026-08-09) - which consumer
    -- instance produced this row (config.ini's consumer_name, e.g.
    -- "FILE_CONSUMER_01") - essential once HTTP consumer exists
    -- alongside File Consumer and a dashboard needs to tell their
    -- traffic apart, and lets two instances of the same consumer type
    -- carry distinct identities too if ever run side by side.
    CONSUMER_NAME              VARCHAR2(64),

    -- Session / Transaction Context
    SESSION_ID                 VARCHAR2(64),
    TRANSACTION_ID              VARCHAR2(64),
    TRANSACTION_NAME            VARCHAR2(128),
    AUDIT_ID                    VARCHAR2(64),

    -- Client Information
    CLIENT_IP                   VARCHAR2(64),

    -- Server Information
    HOST_NAME                   VARCHAR2(128),
    SERVER_IP                   VARCHAR2(64),
    SERVER_PORT                 NUMBER,
    PROCESS_ID                  NUMBER,
    THREAD_ID                   NUMBER,        -- the clean [T%d] worker number
                                                -- (0-4), not a raw OS thread
                                                -- handle - see logger.h's own
                                                -- logger_get_worker_id()

    -- Database Environment
    DATASOURCE_NAME              VARCHAR2(128),
    CONNECTION_ID                NUMBER,
    POOL_ID                      NUMBER,

    -- Operation
    OPERATION                    VARCHAR2(32),
    OBJECT_NAME                  VARCHAR2(128),
    SQL_HASH                     NUMBER,
    CACHE_KEY_HASH                NUMBER,

    -- Timing - START_TIME_TS is the canonical "when did this happen"
    -- column, and the one this table is partitioned on (see
    -- PARTITION_DATE below). Despite the "_us" suffix on the C struct
    -- fields these come from, they're actually formatted timestamp
    -- strings (not raw microsecond integers) - a pre-existing naming
    -- quirk in metrics_record_t, not something introduced here.
    START_TIME_TS                 TIMESTAMP,
    END_TIME_TS                   TIMESTAMP,
    CACHE_LOOKUP_US                NUMBER,
    LEVEL1_PARSE_US                NUMBER,
    LEVEL2_PARSE_US                NUMBER,
    SQL_PARSE_US                   NUMBER,
    EXECUTION_US                   NUMBER,
    TOTAL_US                       NUMBER,

    -- Volume / Result
    ROWS_AFFECTED                  NUMBER,
    OUTPUT_XML_BYTES                NUMBER,
    CLOB_BYTES                      NUMBER,
    LOB_BYTES                       NUMBER,
    BYTES_PROCESSED                 NUMBER,
    CACHE_HIT                       NUMBER(1),
    STATUS_CODE                     NUMBER,
    ERROR_CODE                      VARCHAR2(64),
    ERROR_TEXT                      VARCHAR2(255),

    -- Connection Overhead
    CONNECTION_WAIT_US               NUMBER,
    CONNECTION_CREATE_US             NUMBER,
    CONNECTION_ACQUIRE_US            NUMBER,

    -- When this row was actually written to the table - distinct from
    -- START_TIME_TS (when the operation itself started). Useful for
    -- confirming the async writer's own timing (Stage 2) independent
    -- of the operation's own duration.
    INSERTED_TS                      TIMESTAMP      DEFAULT SYSTIMESTAMP NOT NULL,

    -- Partition key (mirrors OCI_SESSION's own pattern, closure item 5
    -- follow-up) - partitioned on START_TIME_TS, the operation's own
    -- timestamp, not INSERTED_TS - "recent activity" dashboard queries
    -- care about when the request happened, not when the async writer
    -- got around to persisting it (normally the same minute, but not
    -- guaranteed under load).
    PARTITION_DATE                   DATE           GENERATED ALWAYS AS
                                        (CAST(START_TIME_TS AS DATE)) VIRTUAL
)
TABLESPACE USERS
PARTITION BY RANGE (PARTITION_DATE)
INTERVAL (NUMTOYMINTERVAL(1, 'MONTH'))
(
    -- Seed partition - Oracle auto-creates monthly partitions beyond this
    PARTITION P_INITIAL VALUES LESS THAN (DATE '2026-01-01')
)
STORAGE (
    INITIAL     10M
    NEXT        10M
    PCTINCREASE 0
);

-- Terry's own indexing requirement (2026-08-09): date/time and
-- operation. Two separate indexes rather than one composite - a
-- dashboard's actual query shapes aren't fully known yet ("recent
-- activity" alone vs. "activity for operation X" vs. both together),
-- and two single-purpose indexes let Oracle use either independently
-- or combine them, rather than committing to one fixed column order
-- now that might not match how queries actually end up being written.
CREATE INDEX IDX_METRICS_START_TIME
    ON OCI_METRICS (START_TIME_TS)
    LOCAL;

CREATE INDEX IDX_METRICS_OPERATION_TIME
    ON OCI_METRICS (OPERATION, START_TIME_TS)
    LOCAL;

-- Common dashboard query pattern: which consumer's traffic, over time -
-- essential once HTTP consumer exists alongside File Consumer.
CREATE INDEX IDX_METRICS_CONSUMER_TIME
    ON OCI_METRICS (CONSUMER_NAME, START_TIME_TS)
    LOCAL;

-- Aggregation (rollups to nightly/weekly/monthly summary tables) is a
-- separate, out-of-scope Oracle-scheduled piece per Terry's own note
-- (2026-08-09) - this table holds raw rows only. Monthly partitioning
-- above is what makes that summary job's own eventual purge of old raw
-- rows cheap (DROP PARTITION, not row-by-row DELETE) once it exists.
