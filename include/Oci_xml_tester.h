
#ifndef OCI_XML_TESTER_H
#define OCI_XML_TESTER_H

#include <OCI_Connection.h>
#include <OCI_Execute_Query_Module.h>
#include "ini_reader.h"

int list_available_tests(oci_context_t *ctx , 
                         const char *input_dir);

int run_all_xml_tests(oci_context_t *ctx,
                      app_config_t *config);

int execute_xml_test(oci_context_t *ctx,
                     app_config_t *config,
                     const char *filename);

#endif

