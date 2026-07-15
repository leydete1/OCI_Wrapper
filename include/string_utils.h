
#ifndef STRING_UTILS_H
#define STRING_UTILS_H

/*
 * Utility functions for safe string handling
 * Used across OCI tester modules
 */

/* Remove problematic whitespace and normalize SQL strings */
void sanitize_sql(char *sql);

/* Optional helpers you may want later */
void trim_whitespace(char *str);
void remove_nbsp(char *str);

#endif

