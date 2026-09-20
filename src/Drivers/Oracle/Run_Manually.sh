
export LD_LIBRARY_PATH=/home/leyden100/eclipse-workspace/OCI_Wrapper/oci
export LSAN_OPTIONS=suppressions=/home/leyden100/eclipse-workspace/OCI_Wrapper/Debug/lsan_suppressions.txt


#Test stage 1 Driver_Connect_Test
#./Driver_Connect_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/oracle_driver_run.log 2>&1



#Test stage 2 Driver_Pool_Test
#./Driver_Pool_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_Pool_Test.log 2>&1


#Test stage 3 Driver_lob_Select_Test
#./Driver_LOB_Select_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_LOB_Select_Test.log 2>&1


#Test stage 4 Driver_Delete_Test
./Driver_DDL_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_DDL_Test.log 2>&1

echo "EXIT CODE: $?" >> /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/oracle_driver_run.log
cat /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_DDL_Test.log



