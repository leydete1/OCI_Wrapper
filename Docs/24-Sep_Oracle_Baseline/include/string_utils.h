
#ifndef STRING_UTILS_H
#define STRING_UTILS_H

/*
 * Utility functions for safe string handling
 * Used across OCI tester modules
 */

#include "OCI_Connection.h";

/* Remove problematic whitespace and normalize SQL strings */
void sanitize_sql(char *sql);

/* Optional helpers you may want later */
void trim_whitespace(char *str);
void remove_nbsp(char *str);
void trim_sql_inplace(char *str, oci_context_t *ctx);
 
#endif

