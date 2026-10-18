-- ======================================================================
-- MSSQL_Security_Seed.sql   (driver_mssql Stage 1, point S10)
--
-- Security rows copied from Oracle DATA_MANAGER (export of 18 Oct 2026).
-- Read by Create_MSSQL_Test_Objects.sql (:r) - not run on its own.
-- IDs are kept (IDENTITY_INSERT), so every foreign key matches Oracle.
-- Oracle stores '' as NULL, so the empty CSV values are NULL here.
-- All accounts are test accounts. Keep this file on the laptop: it is
-- not uploaded to the project.
-- ======================================================================

SET IDENTITY_INSERT DATA_MANAGER.AUTH_SOURCE ON;
INSERT INTO DATA_MANAGER.AUTH_SOURCE (AUTH_SOURCE_ID, NAME, SOURCE_TYPE, CONFIGURATION, ENABLED, CREATED_TS, MODIFIED_TS) VALUES
 (1, 'Local Database',   'LOCAL', NULL, 'Y', '2026-08-26 20:06:03.602263', '2026-08-26 20:06:03.602263'),
 (2, 'Docker Test LDAP', 'LDAP',
     '{"host":"localhost","port":389,"use_tls":false,"bind_dn_pattern":"uid=%s,ou=people,dc=example,dc=com"}',
     'Y', '2026-08-29 12:05:38.003343', '2026-08-29 12:05:38.003343');
SET IDENTITY_INSERT DATA_MANAGER.AUTH_SOURCE OFF;

SET IDENTITY_INSERT DATA_MANAGER.APP_USER ON;
INSERT INTO DATA_MANAGER.APP_USER (USER_ID, USERNAME, DISPLAY_NAME, AUTH_SOURCE_ID, PASSWORD_HASH, PASSWORD_PARAMS,
                                   ENABLED, LOCKED, FAILED_ATTEMPTS, LOCKED_TS, LAST_LOGIN_TS, CREATED_TS, MODIFIED_TS) VALUES
 (1, 'auth.test.user', 'Auth Test User', 1,
     '$argon2id$v=19$m=65536,t=3,p=1$KUP2QqiaqFo9/Smy5iUnHQ$UrSMwH+4Vsx3DjZeQUvecHjpHOKKiVGkMdGTE7lUWiQ', NULL,
     'Y', 'N', 0, NULL, '2026-10-17 19:57:30.091204', '2026-08-29 09:37:10.692489', '2026-10-17 19:57:30.091204'),
 (2, 'auth.test.disabled', 'Auth Test Disabled User', 1,
     '$argon2id$v=19$m=65536,t=3,p=1$KUP2QqiaqFo9/Smy5iUnHQ$UrSMwH+4Vsx3DjZeQUvecHjpHOKKiVGkMdGTE7lUWiQ', NULL,
     'N', 'N', 0, NULL, NULL, '2026-08-29 09:38:14.857185', '2026-08-29 09:38:14.857185'),
 (3, 'auth.test.nearlock', 'Auth Test Near-Lockout User', 1,
     '$argon2id$v=19$m=65536,t=3,p=1$KUP2QqiaqFo9/Smy5iUnHQ$UrSMwH+4Vsx3DjZeQUvecHjpHOKKiVGkMdGTE7lUWiQ', NULL,
     'Y', 'Y', 5, '2026-10-17 19:57:25.147892', '2026-08-30 19:47:30.766613', '2026-08-29 09:43:41.589522', '2026-10-17 19:57:25.147892'),
 (4, 'ldap.test.user', 'LDAP Test User', 2, NULL, NULL,
     'Y', 'N', 0, NULL, '2026-09-01 15:02:27.436859', '2026-08-29 12:05:38.046365', '2026-09-01 15:02:27.436859'),
 (5, 'auth.test.locked', 'Auth Test Locked User', 1,
     '$argon2id$v=19$m=65536,t=3,p=1$KUP2QqiaqFo9/Smy5iUnHQ$UrSMwH+4Vsx3DjZeQUvecHjpHOKKiVGkMdGTE7lUWiQ', NULL,
     'Y', 'Y', 5, '2026-10-11 15:43:05.759696', NULL, '2026-10-11 15:43:05.759696', '2026-10-11 15:43:05.759696');
SET IDENTITY_INSERT DATA_MANAGER.APP_USER OFF;

SET IDENTITY_INSERT DATA_MANAGER.ROLE ON;
INSERT INTO DATA_MANAGER.ROLE (ROLE_ID, ROLE_NAME, DESCRIPTION) VALUES
 (1, 'TEST_ROLE', 'Security Module Stage 5 test role');
SET IDENTITY_INSERT DATA_MANAGER.ROLE OFF;

SET IDENTITY_INSERT DATA_MANAGER.PERMISSION ON;
INSERT INTO DATA_MANAGER.PERMISSION (PERMISSION_ID, PERMISSION_CODE, DESCRIPTION) VALUES
 (1, 'CUSTOMER.READ',  'Stage 5 test permission - granted to TEST_ROLE'),
 (2, 'CUSTOMER.WRITE', 'Stage 5 test permission - deliberately NOT granted to TEST_ROLE, to test denial');
SET IDENTITY_INSERT DATA_MANAGER.PERMISSION OFF;

INSERT INTO DATA_MANAGER.USER_ROLE (USER_ID, ROLE_ID) VALUES (1, 1);
INSERT INTO DATA_MANAGER.ROLE_PERMISSION (ROLE_ID, PERMISSION_ID) VALUES (1, 1);

PRINT 'ok      security rows: AUTH_SOURCE 2, APP_USER 5, ROLE 1, PERMISSION 2, USER_ROLE 1, ROLE_PERMISSION 1';
