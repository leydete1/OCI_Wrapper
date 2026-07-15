#ifndef UNIT_TEST_H
#define UNIT_TEST_H

#include <oci.h>
#include "logger.h"
#include <OCI_Connection.h>
#include <OCI_Execute_Query_Module.h>


int run_unit_tests_Back_10_Mar(oci_context_t *ctx, const char *test_sql_file_name);


#endif



