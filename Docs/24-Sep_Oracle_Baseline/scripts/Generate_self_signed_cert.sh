# =============================================================================
# Data_Manager - config.ini
# =============================================================================
# Every parameter understood by load_ini() is listed here.
# Lines beginning with # are comments and are ignored by the parser.
# Adjust values for your environment before running.
# =============================================================================


# -----------------------------------------------------------------------------
# Database credentials
# -----------------------------------------------------------------------------

# HTTP Consumer, Stage 0 (2026-08-14). TLS is mandatory - see
# http_consumer_runner.c - so the cert/key paths below MUST point at
# real PEM files or the daemon will refuse to start (by design, no
# plaintext fallback). Generate a self-signed pair for local testing:
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
-keyout /etc/ssl/certs/http_consumer_key.pem \
-out    /etc/ssl/certs/http_consumer_cert.pem \
-subj   "/CN=localhost"

