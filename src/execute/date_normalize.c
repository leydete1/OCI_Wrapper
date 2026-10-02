/*
 * date_normalize.c
 *
 * See date_normalize.h for the contract and the supported mask
 * elements. Oracle dialect extraction, Stage 1 (2026-09-30).
 *
 * Replaces Level 2's per-value Oracle round trip
 *   SELECT TO_CHAR(TO_DATE(:value, :mask), :canonical) FROM DUAL
 * with a plain-C parser that reproduces TO_DATE / TO_TIMESTAMP's
 * default (non-FX) matching rules for the mask elements clients send.
 * No database, no OCI, no vendor headers - core code.
 *
 * Structure:
 *   1. tokenize_mask()  - the mask becomes a list of elements; problems
 *                         with the mask itself are reported here
 *                         (DATE_NORM_BAD_MASK), before the value is read.
 *   2. parse_value()    - walks the value against the element list.
 *   3. finish()         - defaults, meridian, range and calendar checks,
 *                         then the canonical string.
 *
 * Calendar: Oracle DATE uses the Julian calendar before 15 Oct 1582 and
 * the Gregorian calendar from then on; 5-14 Oct 1582 do not exist and
 * Oracle moves them to 15 Oct rather than rejecting them. The same
 * rules are applied here so decisions and output match.
 *
 * Rules corrected from the differential test (1 Oct 2026, 110/113
 * matching before, 118/120 after the first fixes): spaces in the value
 * are skipped freely, other punctuation matches up to the mask
 * separator's own length; the 1582 gap maps to 15 Oct; HH/HH12 with no
 * AM/PM in the value keeps the hour as written.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "date_normalize.h"

/* ------------------------------------------------------------------ */
/*  Mask elements                                                      */
/* ------------------------------------------------------------------ */
typedef enum {
    EL_SEP = 0,    /* run of punctuation / spaces in the mask          */
    EL_LIT,        /* "quoted text"                                    */
    EL_YYYY, EL_RRRR, EL_YY, EL_RR,
    EL_MM, EL_MON, EL_MONTH,
    EL_DD,
    EL_HH24, EL_HH12,
    EL_MI, EL_SS, EL_FF,
    EL_AMPM
} el_kind_t;

/* Duplicate detection - one element per group (ORA-01810 equivalent). */
typedef enum {
    G_NONE = 0, G_YEAR, G_MONTH, G_DAY, G_HOUR, G_MIN, G_SEC, G_FRAC, G_MER,
    G_COUNT
} el_group_t;

typedef struct {
    el_kind_t kind;
    int       ff_digits;      /* EL_FF: 1-9, or 0 for bare FF (up to 9) */
    char      text[64];       /* EL_LIT text; the element as written, for messages */
} fmt_el_t;

#define MAX_ELEMENTS 48

typedef struct {
    const char *word;
    el_kind_t   kind;
} keyword_t;

/* Order matters: longer words before their prefixes. */
static const keyword_t KEYWORDS[] = {
    { "HH24",  EL_HH24  },
    { "HH12",  EL_HH12  },
    { "HH",    EL_HH12  },
    { "MONTH", EL_MONTH },
    { "MON",   EL_MON   },
    { "MM",    EL_MM    },
    { "MI",    EL_MI    },
    { "YYYY",  EL_YYYY  },
    { "RRRR",  EL_RRRR  },
    { "YY",    EL_YY    },
    { "RR",    EL_RR    },
    { "DD",    EL_DD    },
    { "SS",    EL_SS    },
    { "A.M.",  EL_AMPM  },
    { "P.M.",  EL_AMPM  },
    { "AM",    EL_AMPM  },
    { "PM",    EL_AMPM  },
};

/* Elements that share a prefix with a supported one but mean something
 * else - checked first so "DDD" is not read as "DD" + "D". */
static const char *UNSUPPORTED_PREFIXES[] = { "SSSSS", "DDD", "DAY" };

static el_group_t group_of(el_kind_t k)
{
    switch (k)
    {
        case EL_YYYY: case EL_RRRR: case EL_YY: case EL_RR: return G_YEAR;
        case EL_MM: case EL_MON: case EL_MONTH:             return G_MONTH;
        case EL_DD:                                          return G_DAY;
        case EL_HH24: case EL_HH12:                          return G_HOUR;
        case EL_MI:                                          return G_MIN;
        case EL_SS:                                          return G_SEC;
        case EL_FF:                                          return G_FRAC;
        case EL_AMPM:                                        return G_MER;
        default:                                             return G_NONE;
    }
}

/* Time elements may be left off the end of the value. */
static int is_time_element(el_kind_t k)
{
    return k == EL_HH24 || k == EL_HH12 || k == EL_MI || k == EL_SS ||
           k == EL_FF   || k == EL_AMPM;
}

static void set_reason(char *reason, size_t reason_size, const char *fmt, ...)
{
    if (!reason || reason_size == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, reason_size, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/*  1. Mask tokenizer                                                  */
/* ------------------------------------------------------------------ */
static int tokenize_mask(const char *mask, int is_timestamp,
                         fmt_el_t *els, int *n_out,
                         char *reason, size_t reason_size)
{
    int n = 0;
    int seen[G_COUNT] = {0};
    const char *p = mask;

    while (*p)
    {
        if (n >= MAX_ELEMENTS)
        {
            set_reason(reason, reason_size, "format is too long");
            return DATE_NORM_BAD_MASK;
        }

        fmt_el_t *el = &els[n];
        memset(el, 0, sizeof(*el));

        /* ---- quoted literal ---- */
        if (*p == '"')
        {
            const char *end = strchr(p + 1, '"');
            if (!end)
            {
                set_reason(reason, reason_size,
                           "format has unterminated quoted text");
                return DATE_NORM_BAD_MASK;
            }
            size_t len = (size_t)(end - (p + 1));
            if (len >= sizeof(el->text)) len = sizeof(el->text) - 1;
            el->kind = EL_LIT;
            memcpy(el->text, p + 1, len);
            el->text[len] = '\0';
            p = end + 1;
            n++;
            continue;
        }

        /* ---- separator run ---- */
        if (!isalnum((unsigned char)*p))
        {
            size_t len = 0;
            while (p[len] && p[len] != '"' && !isalnum((unsigned char)p[len]))
                len++;
            el->kind = EL_SEP;
            size_t copy = len < sizeof(el->text) - 1 ? len : sizeof(el->text) - 1;
            memcpy(el->text, p, copy);
            el->text[copy] = '\0';
            p += len;
            n++;
            continue;
        }

        /* ---- keyword ---- */
        int matched = 0;

        for (size_t u = 0; u < sizeof(UNSUPPORTED_PREFIXES) / sizeof(UNSUPPORTED_PREFIXES[0]); u++)
        {
            size_t ul = strlen(UNSUPPORTED_PREFIXES[u]);
            if (strncasecmp(p, UNSUPPORTED_PREFIXES[u], ul) == 0)
            {
                set_reason(reason, reason_size,
                           "unsupported format element '%.*s'", (int)ul, p);
                return DATE_NORM_BAD_MASK;
            }
        }

        if (strncasecmp(p, "FF", 2) == 0)
        {
            el->kind = EL_FF;
            p += 2;
            if (*p >= '1' && *p <= '9')
            {
                el->ff_digits = *p - '0';
                p++;
            }
            if (el->ff_digits)
                snprintf(el->text, sizeof(el->text), "FF%d", el->ff_digits);
            else
                snprintf(el->text, sizeof(el->text), "FF");
            matched = 1;
        }
        else
        {
            for (size_t k = 0; k < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); k++)
            {
                size_t kl = strlen(KEYWORDS[k].word);
                if (strncasecmp(p, KEYWORDS[k].word, kl) == 0)
                {
                    el->kind = KEYWORDS[k].kind;
                    snprintf(el->text, sizeof(el->text), "%s", KEYWORDS[k].word);
                    p += kl;
                    matched = 1;
                    break;
                }
            }
        }

        if (!matched)
        {
            /* Report the whole letter run, e.g. 'DY', 'FX', 'Q'. */
            size_t len = 0;
            while (p[len] && isalnum((unsigned char)p[len]) && len < 16) len++;
            set_reason(reason, reason_size,
                       "unsupported format element '%.*s'", (int)len, p);
            return DATE_NORM_BAD_MASK;
        }

        /* ---- per-element mask rules ---- */
        el_group_t g = group_of(el->kind);
        if (g != G_NONE)
        {
            if (seen[g])
            {
                set_reason(reason, reason_size,
                           "format element '%s' appears twice (or clashes "
                           "with another element for the same field)",
                           el->text);
                return DATE_NORM_BAD_MASK;
            }
            seen[g] = 1;
        }

        if (el->kind == EL_FF && !is_timestamp)
        {
            set_reason(reason, reason_size,
                       "fractional seconds (FF) are only valid for "
                       "TIMESTAMP columns");
            return DATE_NORM_BAD_MASK;
        }

        n++;
    }

    /* HH24 and a meridian cannot be combined (ORA-01818 equivalent). */
    int has_hh24 = 0, has_ampm = 0, has_data = 0;
    for (int i = 0; i < n; i++)
    {
        if (els[i].kind == EL_HH24) has_hh24 = 1;
        if (els[i].kind == EL_AMPM) has_ampm = 1;
        if (els[i].kind != EL_SEP && els[i].kind != EL_LIT) has_data = 1;
    }
    if (has_hh24 && has_ampm)
    {
        set_reason(reason, reason_size,
                   "HH24 cannot be used with AM/PM");
        return DATE_NORM_BAD_MASK;
    }
    if (!has_data)
    {
        set_reason(reason, reason_size,
                   "format has no date or time elements");
        return DATE_NORM_BAD_MASK;
    }

    *n_out = n;
    return DATE_NORM_OK;
}

/* ------------------------------------------------------------------ */
/*  2. Value parser                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    int year, month, day, hour, minute, second;
    int year_set, month_set, day_set, hour_set;
    int hour_is_12;       /* hour came from HH / HH12            */
    int meridian;         /* 0 none, 1 AM, 2 PM                   */
    char frac[10];        /* fractional digits as read, max 9     */
} dt_parts_t;

static const char *MONTH_FULL[12] = {
    "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
    "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"
};

/* Full name first, then the three-letter abbreviation. Returns 1-12 and
 * advances *pv, or 0 if neither matches. */
static int read_month_name(const char **pv)
{
    const char *v = *pv;
    for (int m = 0; m < 12; m++)
    {
        size_t fl = strlen(MONTH_FULL[m]);
        if (strncasecmp(v, MONTH_FULL[m], fl) == 0)
        {
            *pv = v + fl;
            return m + 1;
        }
    }
    for (int m = 0; m < 12; m++)
    {
        if (strncasecmp(v, MONTH_FULL[m], 3) == 0)
        {
            *pv = v + 3;
            return m + 1;
        }
    }
    return 0;
}

/* Up to max_width digits. Returns the number of digits read (0 = none). */
static int read_digits(const char **pv, int max_width, int *value)
{
    const char *v = *pv;
    int n = 0, acc = 0;
    while (n < max_width && isdigit((unsigned char)v[n]))
    {
        acc = acc * 10 + (v[n] - '0');
        n++;
    }
    *pv = v + n;
    *value = acc;
    return n;
}

/* Two-digit year rules. YY: current century. RR: the 50-year window. */
static int expand_two_digit_year(int yy, int is_rr, int current_year)
{
    int century = current_year / 100;
    if (!is_rr)
        return century * 100 + yy;

    int cur_yy = current_year % 100;
    if (yy < 50)
        return (cur_yy < 50 ? century : century + 1) * 100 + yy;
    return (cur_yy < 50 ? century - 1 : century) * 100 + yy;
}

static int parse_value(const char *value, const fmt_el_t *els, int n,
                       int current_year, dt_parts_t *dt,
                       char *reason, size_t reason_size)
{
    const char *v = value;

    /* Leading blanks are ignored. */
    while (isspace((unsigned char)*v)) v++;

    for (int i = 0; i < n; i++)
    {
        const fmt_el_t *el = &els[i];

        if (*v == '\0')
        {
            /* The value ran out. Fine only if everything left in the
             * mask is separators, literals or time elements. */
            for (int j = i; j < n; j++)
            {
                if (els[j].kind == EL_SEP || els[j].kind == EL_LIT) continue;
                if (is_time_element(els[j].kind)) continue;
                set_reason(reason, reason_size,
                           "input value not long enough for date format");
                return DATE_NORM_BAD_VALUE;
            }
            break;
        }

        int num = 0, nd = 0;

        switch (el->kind)
        {
            case EL_SEP:
            {
                /* Oracle's rule, from the differential test (1 Oct):
                 *   - whitespace in the value is skipped freely
                 *     ("25 /12/2026", "2026-06-15  10:00:00" accepted);
                 *   - other punctuation counts: at most as many
                 *     characters as the mask separator has
                 *     ("25//12//2026" against DD/MM/YYYY rejected,
                 *     ORA-01858). Which punctuation does not matter.
                 *   - none at all is fine (full-width fields). */
                size_t max_punct = strlen(el->text);
                size_t punct = 0;
                while (*v && !isalnum((unsigned char)*v))
                {
                    if (isspace((unsigned char)*v)) { v++; continue; }
                    if (punct >= max_punct) break;
                    punct++;
                    v++;
                }
                break;
            }

            case EL_LIT:
            {
                size_t ll = strlen(el->text);
                if (strncasecmp(v, el->text, ll) != 0)
                {
                    set_reason(reason, reason_size,
                               "literal does not match format string");
                    return DATE_NORM_BAD_VALUE;
                }
                v += ll;
                break;
            }

            case EL_YYYY:
            case EL_RRRR:
            case EL_YY:
            case EL_RR:
                nd = read_digits(&v, 4, &num);
                if (nd == 0) goto non_numeric;
                if (nd <= 2 && el->kind != EL_YYYY)
                    num = expand_two_digit_year(num,
                                                el->kind == EL_RR || el->kind == EL_RRRR,
                                                current_year);
                dt->year = num;
                dt->year_set = 1;
                break;

            case EL_MM:
                if (isalpha((unsigned char)*v))
                {
                    /* Alternative element: MM also accepts MON/MONTH. */
                    int m = read_month_name(&v);
                    if (m == 0) goto bad_month;
                    dt->month = m;
                }
                else
                {
                    nd = read_digits(&v, 2, &num);
                    if (nd == 0) goto non_numeric;
                    dt->month = num;
                }
                dt->month_set = 1;
                break;

            case EL_MON:
            case EL_MONTH:
            {
                int m = read_month_name(&v);
                if (m == 0) goto bad_month;
                dt->month = m;
                dt->month_set = 1;
                break;
            }

            case EL_DD:
                nd = read_digits(&v, 2, &num);
                if (nd == 0) goto non_numeric;
                dt->day = num;
                dt->day_set = 1;
                break;

            case EL_HH24:
            case EL_HH12:
                nd = read_digits(&v, 2, &num);
                if (nd == 0) goto non_numeric;
                dt->hour = num;
                dt->hour_set = 1;
                dt->hour_is_12 = (el->kind == EL_HH12);
                break;

            case EL_MI:
                nd = read_digits(&v, 2, &num);
                if (nd == 0) goto non_numeric;
                dt->minute = num;
                break;

            case EL_SS:
                nd = read_digits(&v, 2, &num);
                if (nd == 0) goto non_numeric;
                dt->second = num;
                break;

            case EL_FF:
            {
                int width = el->ff_digits ? el->ff_digits : 9;
                int k = 0;
                while (k < width && isdigit((unsigned char)v[k]))
                {
                    dt->frac[k] = v[k];
                    k++;
                }
                dt->frac[k] = '\0';
                if (k == 0) goto non_numeric;
                v += k;
                break;
            }

            case EL_AMPM:
                if      (strncasecmp(v, "A.M.", 4) == 0) { dt->meridian = 1; v += 4; }
                else if (strncasecmp(v, "P.M.", 4) == 0) { dt->meridian = 2; v += 4; }
                else if (strncasecmp(v, "AM",   2) == 0) { dt->meridian = 1; v += 2; }
                else if (strncasecmp(v, "PM",   2) == 0) { dt->meridian = 2; v += 2; }
                else
                {
                    set_reason(reason, reason_size,
                               "AM/A.M. or PM/P.M. required");
                    return DATE_NORM_BAD_VALUE;
                }
                break;
        }
    }

    /* Trailing blanks are ignored; anything else left over is an error. */
    while (isspace((unsigned char)*v)) v++;
    if (*v)
    {
        set_reason(reason, reason_size,
                   "date format picture ends before converting entire "
                   "input string");
        return DATE_NORM_BAD_VALUE;
    }
    return DATE_NORM_OK;

non_numeric:
    set_reason(reason, reason_size,
               "a non-numeric character was found where a numeric was "
               "expected");
    return DATE_NORM_BAD_VALUE;

bad_month:
    set_reason(reason, reason_size, "not a valid month");
    return DATE_NORM_BAD_VALUE;
}

/* ------------------------------------------------------------------ */
/*  3. Checks and output                                               */
/* ------------------------------------------------------------------ */
static int is_leap(int y)
{
    if (y < 1582) return (y % 4) == 0;                 /* Julian    */
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; /* Gregorian */
}

static int days_in_month(int y, int m)
{
    static const int DIM[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && is_leap(y)) return 29;
    return DIM[m - 1];
}

static int finish(dt_parts_t *dt, int is_timestamp,
                  int current_year, int current_month,
                  char *out, size_t out_size,
                  char *reason, size_t reason_size)
{
    /* Oracle defaults: current year and month, day 1, midnight. */
    if (!dt->year_set)  dt->year  = current_year;
    if (!dt->month_set) dt->month = current_month;
    if (!dt->day_set)   dt->day   = 1;

    if (dt->year < 1 || dt->year > 9999)
    {
        set_reason(reason, reason_size,
                   "year must be between 1 and 9999");
        return DATE_NORM_BAD_VALUE;
    }
    if (dt->month < 1 || dt->month > 12)
    {
        set_reason(reason, reason_size, "not a valid month");
        return DATE_NORM_BAD_VALUE;
    }
    if (dt->day < 1 || dt->day > 31)
    {
        set_reason(reason, reason_size,
                   "day of month must be between 1 and last day of month");
        return DATE_NORM_BAD_VALUE;
    }
    if (dt->day > days_in_month(dt->year, dt->month))
    {
        set_reason(reason, reason_size, "date not valid for month specified");
        return DATE_NORM_BAD_VALUE;
    }
    /* 5-14 Oct 1582 do not exist (Julian -> Gregorian switch). Oracle
     * does not reject them: TO_DATE('10/10/1582','DD/MM/YYYY') gives
     * 1582-10-15 (differential test, 1 Oct). Same here. */
    if (dt->year == 1582 && dt->month == 10 && dt->day >= 5 && dt->day <= 14)
        dt->day = 15;

    if (dt->hour_set)
    {
        if (dt->hour_is_12)
        {
            if (dt->hour < 1 || dt->hour > 12)
            {
                set_reason(reason, reason_size,
                           "hour must be between 1 and 12");
                return DATE_NORM_BAD_VALUE;
            }
            /* 12 AM is 00 and 12 PM is 12. With no meridian in the
             * value the hour is taken as written (12:15 stays 12:15),
             * as Oracle does (differential test, 1 Oct). */
            if (dt->meridian == 1)
                dt->hour = dt->hour % 12;
            else if (dt->meridian == 2)
                dt->hour = dt->hour % 12 + 12;
        }
        else if (dt->hour > 23)
        {
            set_reason(reason, reason_size, "hour must be between 0 and 23");
            return DATE_NORM_BAD_VALUE;
        }
    }
    if (dt->minute > 59)
    {
        set_reason(reason, reason_size, "minutes must be between 0 and 59");
        return DATE_NORM_BAD_VALUE;
    }
    if (dt->second > 59)
    {
        set_reason(reason, reason_size, "seconds must be between 0 and 59");
        return DATE_NORM_BAD_VALUE;
    }

    int w;
    if (is_timestamp)
    {
        /* Six digits, truncated, right-padded with zeros. */
        char f6[7] = "000000";
        for (int k = 0; k < 6 && dt->frac[k]; k++) f6[k] = dt->frac[k];
        w = snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d:%02d.%s",
                     dt->year, dt->month, dt->day,
                     dt->hour, dt->minute, dt->second, f6);
    }
    else
    {
        w = snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d:%02d",
                     dt->year, dt->month, dt->day,
                     dt->hour, dt->minute, dt->second);
    }

    if (w < 0 || (size_t)w >= out_size)
    {
        set_reason(reason, reason_size, "output buffer too small");
        return DATE_NORM_BAD_ARG;
    }
    return DATE_NORM_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
int date_normalize(const char *value,
                   const char *mask,
                   int         is_timestamp,
                   char       *out,
                   size_t      out_size,
                   char       *reason,
                   size_t      reason_size)
{
    if (reason && reason_size) reason[0] = '\0';

    if (!value || !out || out_size < DATE_NORMALIZE_OUT_MAX)
    {
        set_reason(reason, reason_size, "invalid arguments");
        return DATE_NORM_BAD_ARG;
    }

    if (!mask || !mask[0])
        mask = is_timestamp ? DATE_CANONICAL_MASK_TS : DATE_CANONICAL_MASK;

    fmt_el_t els[MAX_ELEMENTS];
    int      n = 0;

    int rc = tokenize_mask(mask, is_timestamp, els, &n, reason, reason_size);
    if (rc != DATE_NORM_OK) return rc;

    time_t    now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int current_year  = tm_now.tm_year + 1900;
    int current_month = tm_now.tm_mon + 1;

    dt_parts_t dt;
    memset(&dt, 0, sizeof(dt));

    rc = parse_value(value, els, n, current_year, &dt, reason, reason_size);
    if (rc != DATE_NORM_OK) return rc;

    char tmp[DATE_NORMALIZE_OUT_MAX];
    rc = finish(&dt, is_timestamp, current_year, current_month,
                tmp, sizeof(tmp), reason, reason_size);
    if (rc != DATE_NORM_OK) return rc;

    /* Only touch out on success. */
    snprintf(out, out_size, "%s", tmp);
    return DATE_NORM_OK;
}

int date_mask_is_canonical(const char *mask)
{
    if (!mask) return 0;
    while (isspace((unsigned char)*mask)) mask++;

    size_t len = strlen(mask);
    while (len > 0 && isspace((unsigned char)mask[len - 1])) len--;

    return len == strlen(DATE_CANONICAL_MASK) &&
           strncasecmp(mask, DATE_CANONICAL_MASK, len) == 0;
}
