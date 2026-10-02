/*
 * date_normalize.h
 *
 * Oracle dialect extraction, Stage 1 (2026-09-30).
 *
 * Validates a client date/timestamp string and rewrites it into the
 * core's one canonical form, in plain C, with no database round trip:
 *
 *   DATE       YYYY-MM-DD HH:MM:SS           e.g. 2026-12-25 00:00:00
 *   TIMESTAMP  YYYY-MM-DD HH:MM:SS.ffffff    e.g. 2026-12-25 00:00:00.000000
 *
 * This is ISO 8601 with a space separator, and it is byte-identical to
 * what the old Oracle round trip produced with nls_date_format =
 * 'YYYY-MM-DD HH24:MI:SS' (and '.FF6' for TIMESTAMP) - see
 * DATE_CANONICAL_MASK and date_mask_is_canonical() below.
 *
 * The source format is an Oracle-style mask - the wire contract for
 * <client_date_format> is unchanged. When no mask is given, the value
 * is validated against the canonical mask itself (today's behaviour).
 *
 * Supported mask elements (case-insensitive):
 *   YYYY RRRR YY RR          year
 *   MM MON MONTH             month (English names; MM also accepts a
 *                            name and MON/MONTH accept either form -
 *                            Oracle's alternative-element rule)
 *   DD                       day of month
 *   HH24 HH HH12             hour (HH = HH12)
 *   MI SS                    minute, second
 *   FF FF1..FF9              fractional seconds (TIMESTAMP only)
 *   AM PM A.M. P.M.          meridian (all four are interchangeable)
 *   "text"                   quoted literal
 *   punctuation / spaces     separators
 * Anything else (DY, DAY, DDD, J, Q, W, SSSSS, FX, FM, TZH, ...) is
 * rejected as an unsupported element rather than guessed at.
 *
 * Leniency follows Oracle's default (non-FX) TO_DATE rules:
 *   - numeric fields may have fewer digits than their width when a
 *     separator follows ("5/1/2026" with DD/MM/YYYY);
 *   - any punctuation in the value matches a separator in the mask
 *     ("25-12-2026", "25.12.2026" with DD/MM/YYYY), up to the
 *     separator's own length ("25//12//2026" is rejected); spaces are
 *     skipped freely ("25 /12/2026"); and separators may be omitted
 *     when fields are full width ("25122026");
 *   - trailing TIME elements may be omitted ("2026-06-15" against the
 *     canonical mask gives 00:00:00);
 *   - missing year/month default to the current year/month and a
 *     missing day defaults to 1, as Oracle does.
 * The differential test in Driver_Level2Parser_Test.c compares every
 * one of these rules against the real Oracle TO_DATE/TO_TIMESTAMP.
 */

#ifndef DATE_NORMALIZE_H
#define DATE_NORMALIZE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The one mask the canonical output corresponds to. Until Stage 2
 * moves the TO_DATE()/TO_TIMESTAMP() bind wrappers into the driver,
 * those wrappers still use ctx->ini->nls_date_format, so Level 2 must
 * check that nls_date_format still equals this mask
 * (date_mask_is_canonical()) - otherwise normalised values and the
 * wrappers would disagree. */
#define DATE_CANONICAL_MASK      "YYYY-MM-DD HH24:MI:SS"
#define DATE_CANONICAL_MASK_TS   "YYYY-MM-DD HH24:MI:SS.FF6"

/* Output buffer size that always fits the canonical form + NUL. */
#define DATE_NORMALIZE_OUT_MAX   32

#define DATE_NORM_OK              0
#define DATE_NORM_BAD_VALUE      -1   /* value does not match the mask    */
#define DATE_NORM_BAD_MASK       -2   /* the mask itself is not supported */
#define DATE_NORM_BAD_ARG        -3   /* NULL pointers / tiny buffers     */

/*
 * date_normalize()
 *
 *   value         the client's string (not modified)
 *   mask          Oracle-style mask, or NULL/"" for the canonical mask
 *                 (DATE_CANONICAL_MASK, or DATE_CANONICAL_MASK_TS when
 *                 is_timestamp)
 *   is_timestamp  1 for TIMESTAMP-class columns, 0 for DATE
 *   out           receives the canonical string on success
 *   reason        on failure, a plain-English reason with no database
 *                 error code, e.g. "not a valid month" (the 0.4
 *                 decision). Always NUL-terminated when reason_size > 0.
 *
 * Returns DATE_NORM_OK, DATE_NORM_BAD_VALUE, DATE_NORM_BAD_MASK or
 * DATE_NORM_BAD_ARG. Idempotent: feeding the output back in with no
 * mask returns the same string.
 */
int date_normalize(const char *value,
                   const char *mask,
                   int         is_timestamp,
                   char       *out,
                   size_t      out_size,
                   char       *reason,
                   size_t      reason_size);

/* 1 if mask is DATE_CANONICAL_MASK (case-insensitive, surrounding
 * whitespace ignored), else 0. */
int date_mask_is_canonical(const char *mask);

#ifdef __cplusplus
}
#endif

#endif /* DATE_NORMALIZE_H */
