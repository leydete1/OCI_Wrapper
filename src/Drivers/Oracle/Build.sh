
  
    
    
    gcc -I/home/leyden100/eclipse-workspace/OCI_Wrapper/oci/instantclient-sdk-linux.x64-23.26.1.0.0/instantclient_23_26/sdk/include \
    -I/usr/include/cjson \
    -I/usr/include/libxml2 \
    -I/home/leyden100/eclipse-workspace/OCI_Wrapper/include \
    -I. \
    -O0 -g3 -Wall -fmessage-length=0 -fsanitize=address -fno-omit-frame-pointer \
    -o Driver_Procedure_Test \
     Driver_Procedure_Test.c db_driver.c driver_oracle.c  \
     OCI_Audit_Trail_Manager.c OCI_Connection.c oci_cache.c OCI_Connection_Pool.c string_utils.c \
     ini_reader.c logger.c metrics.c  ctx_utils.c OCI_Table_Metadata_Module.c OCI_Delete_Execute_Module.c\
      OCI_Blob_Utils.c OCI_Clob_Utils.c  XML_Helper.c OCI_Resultset_Builder.c\
      metadata_cache.c OCI_Execute_Query_Batch_Module.c OCI_Level2_Parser.c OCI_Transaction_Manager.c\
      metrics_writer.c OCI_Insert_Execute_Module.c OCI_Response_Writer.c resultset_cache.c\
      generic_queue.c OCI_DDL_Create_Table_Module.c OCI_DDL_Create_View_Module.c OCI_DDL_Execute_Module.c \
      sql_dependency_extractor.c \
      OCI_DDL_Create_Procedure_Module.c  OCI_DDL_Create_User_Module.c   OCI_DDL_Drop_Table_Module.c   OCI_DDL_Grant_Module.c    OCI_Insert_Validate_Module.c \
    -L/home/leyden100/eclipse-workspace/OCI_Wrapper/oci \
    -Wl,--start-group -lclntsh -lldap -lsodium -lmicrohttpd -lcjson -lxml2 -lclntshcore -lnnz -lcurl -lpthread -lm -Wl,--end-group


