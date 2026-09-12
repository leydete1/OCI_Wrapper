gcc -I/home/leyden100/eclipse-workspace/OCI_Wrapper/oci/instantclient-sdk-linux.x64-23.26.1.0.0/instantclient_23_26/sdk/include \
    -I/usr/include/cjson \
    -I/usr/include/libxml2 \
    -I/home/leyden100/eclipse-workspace/OCI_Wrapper/include \
    -I. \
    -O0 -g3 -Wall -fmessage-length=0 -fsanitize=address -fno-omit-frame-pointer \
    -o Driver_Connect_Test \
    Driver_Connect_Test.c db_driver.c driver_oracle.c \
    OCI_Connection.c oci_cache.c OCI_Connection_Pool.c string_utils.c ini_reader.c logger.c metrics.c ctx_utils.c OCI_Table_Metadata_Module.c OCI_Resultset_Builder.c\
    -L/home/leyden100/eclipse-workspace/OCI_Wrapper/oci \
    -Wl,--start-group -lclntsh -lldap -lsodium -lmicrohttpd -lcjson -lxml2 -lclntshcore -lnnz -lcurl -lpthread -lm -Wl,--end-group
    
    
    
    
    gcc -I/home/leyden100/eclipse-workspace/OCI_Wrapper/oci/instantclient-sdk-linux.x64-23.26.1.0.0/instantclient_23_26/sdk/include \
    -I/usr/include/cjson \
    -I/usr/include/libxml2 \
    -I/home/leyden100/eclipse-workspace/OCI_Wrapper/include \
    -I. \
    -O0 -g3 -Wall -fmessage-length=0 -fsanitize=address -fno-omit-frame-pointer \
    -o Driver_Pool_Test \
    Driver_Pool_Test.c db_driver.c driver_oracle.c  \
    OCI_Connection.c oci_cache.c OCI_Connection_Pool.c string_utils.c ini_reader.c logger.c metrics.c  ctx_utils.c OCI_Table_Metadata_Module.c OCI_Resultset_Builder.c\
    -L/home/leyden100/eclipse-workspace/OCI_Wrapper/oci \
    -Wl,--start-group -lclntsh -lldap -lsodium -lmicrohttpd -lcjson -lxml2 -lclntshcore -lnnz -lcurl -lpthread -lm -Wl,--end-group
    
    Driver_Select_Test.c
    
    
    
    gcc -I/home/leyden100/eclipse-workspace/OCI_Wrapper/oci/instantclient-sdk-linux.x64-23.26.1.0.0/instantclient_23_26/sdk/include \
    -I/usr/include/cjson \
    -I/usr/include/libxml2 \
    -I/home/leyden100/eclipse-workspace/OCI_Wrapper/include \
    -I. \
    -O0 -g3 -Wall -fmessage-length=0 -fsanitize=address -fno-omit-frame-pointer \
    -o Driver_Select_Test \
     db_driver.c driver_oracle.c  \
    Driver_Select_Test.c OCI_Connection.c oci_cache.c OCI_Connection_Pool.c string_utils.c ini_reader.c logger.c metrics.c  ctx_utils.c OCI_Table_Metadata_Module.c OCI_Resultset_Builder.c \
    -L/home/leyden100/eclipse-workspace/OCI_Wrapper/oci \
    -Wl,--start-group -lclntsh -lldap -lsodium -lmicrohttpd -lcjson -lxml2 -lclntshcore -lnnz -lcurl -lpthread -lm -Wl,--end-group


