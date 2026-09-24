#!/bin/bash
#
# test_check_permission.sh
#
# Security Module Stage 5 - end-to-end CHECK_PERMISSION test.
#
# WHY THIS EXISTS AS ITS OWN SCRIPT, NOT A Run.sh FIXTURE:
# Run.sh's http_consumer_tester creates ONE shared session via a plain
# CREATE_SESSION at the start of the whole run and splices that same
# session_id into every fixture automatically - but
# authz_build_permission_cache() only runs inside auth_authenticate()
# (Stage 5), never for a plain CREATE_SESSION. That shared session_id
# would always have an empty/missing permission cache entry, so every
# CHECK_PERMISSION fixture run through the normal tester would come
# back DENIED regardless of what Seed_Authz_Test_Data.txt grants -
# not a useful test of the actual ALLOWED path.
#
# This script does the two calls in the correct order instead:
#   1. AUTHENTICATE as auth.test.user - this builds a REAL permission
#      cache entry for the session_id it returns.
#   2. CHECK_PERMISSION twice, using that real session_id - once for
#      CUSTOMER.READ (granted - expect ALLOWED) and once for
#      CUSTOMER.WRITE (deliberately not granted - expect DENIED).
#
# Requires Seed_Auth_Test_Users.txt (Stage 2) AND
# Seed_Authz_Test_Data.txt (Stage 5) to have both been run first, and
# the Docker OpenLDAP container is NOT required (auth.test.user is a
# LOCAL-source account).
#
# Usage:
#   ./test_check_permission.sh [base_url]
#   (defaults to https://localhost:8443)

BASE_URL="${1:-https://localhost:8443}"

echo "=================================================================="
echo "Security Module Stage 5 - CHECK_PERMISSION end-to-end test"
echo "base_url=$BASE_URL"
echo "=================================================================="
echo

# ---------------------------------------------------------------------
# Step 1: AUTHENTICATE - builds the real permission cache entry.
# ---------------------------------------------------------------------
echo "[1/3] Authenticating as auth.test.user ..."

AUTH_RESPONSE=$(curl -sk -X POST "$BASE_URL/" \
    -H "Content-Type: application/json" \
    --data-binary '{
        "external_audit_id": "test_check_permission_authenticate",
        "session_id": "-",
        "transaction": {
            "required": 0,
            "operations": [
                { "type": "AUTHENTICATE",
                  "username": "auth.test.user",
                  "credential": "Str0ng!TestPass2026" }
            ]
        }
    }')

SESSION_ID=$(echo "$AUTH_RESPONSE" | grep -o '"session_id":"[^"]*"' | head -1 | cut -d'"' -f4)

if [ -z "$SESSION_ID" ]; then
    echo "FAIL - could not authenticate or extract session_id."
    echo "Full response:"
    echo "$AUTH_RESPONSE"
    exit 1
fi

echo "OK - session_id=$SESSION_ID"
echo

# ---------------------------------------------------------------------
# Step 2: CHECK_PERMISSION for CUSTOMER.READ - granted, expect ALLOWED.
# ---------------------------------------------------------------------
echo "[2/3] CHECK_PERMISSION CUSTOMER.READ (expect ALLOWED) ..."

READ_RESPONSE=$(curl -sk -X POST "$BASE_URL/" \
    -H "Content-Type: application/json" \
    --data-binary "{
        \"external_audit_id\": \"test_check_permission_read\",
        \"session_id\": \"$SESSION_ID\",
        \"transaction\": {
            \"required\": 0,
            \"operations\": [
                { \"type\": \"CHECK_PERMISSION\",
                  \"permission_code\": \"CUSTOMER.READ\" }
            ]
        }
    }")

if echo "$READ_RESPONSE" | grep -q '"result":"ALLOWED"'; then
    echo "PASS - CUSTOMER.READ correctly ALLOWED"
else
    echo "FAIL - expected ALLOWED, got:"
    echo "$READ_RESPONSE"
fi
echo

# ---------------------------------------------------------------------
# Step 3: CHECK_PERMISSION for CUSTOMER.WRITE - ungranted, expect
# DENIED.
# ---------------------------------------------------------------------
echo "[3/3] CHECK_PERMISSION CUSTOMER.WRITE (expect DENIED) ..."

WRITE_RESPONSE=$(curl -sk -X POST "$BASE_URL/" \
    -H "Content-Type: application/json" \
    --data-binary "{
        \"external_audit_id\": \"test_check_permission_write\",
        \"session_id\": \"$SESSION_ID\",
        \"transaction\": {
            \"required\": 0,
            \"operations\": [
                { \"type\": \"CHECK_PERMISSION\",
                  \"permission_code\": \"CUSTOMER.WRITE\" }
            ]
        }
    }")

if echo "$WRITE_RESPONSE" | grep -q '"result":"DENIED"'; then
    echo "PASS - CUSTOMER.WRITE correctly DENIED"
else
    echo "FAIL - expected DENIED, got:"
    echo "$WRITE_RESPONSE"
fi
echo

echo "=================================================================="
echo "Done. Cross-check security_Data_Manager.log for the matching"
echo "ALLOWED/DENIED lines from authz_has_permission()."
echo "=================================================================="
