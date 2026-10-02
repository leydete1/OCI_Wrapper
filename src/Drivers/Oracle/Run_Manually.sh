#leyden100@Terry-HP-Laptop-15-fd0xxx:~/eclipse-workspace/OCI_Wrapper/src/Drivers/Oracle$ ls -ltr
#total 2320
#-rw-rw-r-- 1 leyden100 leyden100    9367 Sep 11 20:21 Driver_Pool_Test.c
#-rw-rw-r-- 1 leyden100 leyden100   13778 Sep 13 21:13 Driver_Select_Test.c
#-rw-rw-r-- 1 leyden100 leyden100   22540 Sep 13 21:13 Driver_LOB_Select_Test.c
#-rw-rw-r-- 1 leyden100 leyden100   32454 Sep 19 16:00 Driver_Insert_Test.c
#-rw-rw-r-- 1 leyden100 leyden100   29079 Sep 20 12:30 Driver_Procedure_Test.c
#-rw-rw-r-- 1 leyden100 leyden100   27425 Sep 20 20:03 Driver_Update_Test.c
#-rw-rw-r-- 1 leyden100 leyden100   18238 Sep 20 20:03 Driver_Delete_Test.c
#-rw-rw-r-- 1 leyden100 leyden100   12945 Sep 20 20:21 Driver_DDL_Test.c
#-rw-rw-r-- 1 leyden100 leyden100   21301 Sep 21 21:51 Driver_Metadata_Test.c
#-rw-rw-r-- 1 leyden100 leyden100   21344 Sep 22 15:53 Driver_Level2Parser_Test.c
#-rw-rw-r-- 1 leyden100 leyden100    7892 Sep 22 21:42 Driver_Connect_Test.c
#-rwxrwxrwx 1 leyden100 leyden100     934 Sep 23 16:00 Run_Manually.sh
#-rw-rw-r-- 1 leyden100 leyden100   27488 Sep 27 21:23 Driver_Transaction_Test.c
#-rwxrwxrwx 1 leyden100 leyden100    1468 Sep 28 22:20 Build.sh
#-rwxrwxr-x 1 leyden100 leyden100 2096096 Sep 28 22:35 Driver_Transaction_Test
#l
export LD_LIBRARY_PATH=/home/leyden100/eclipse-workspace/OCI_Wrapper/oci
export LSAN_OPTIONS=suppressions=/home/leyden100/eclipse-workspace/OCI_Wrapper/Debug/lsan_suppressions.txt


#Test runs
./Driver_Pool_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_Pool_Test.log 2>&1
./Driver_Select_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_Select_Test.log 2>&1
./Driver_LOB_Select_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_LOB_Select_Test.log 2>&1
./Driver_Insert_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_Insert_Test.log 2>&1
./Driver_Update_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_Update_Test.log 2>&1
./Driver_Delete_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_Delete_Test.log 2>&1
./Driver_DDL_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_DDL_Test.log 2>&1
./Driver_Metadata_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_Metadata_Test.log 2>&1
./Driver_Level2Parser_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_Level2Parser_Test.log 2>&1
./Driver_Connect_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_Connect_Test.log 2>&1
./Driver_Transaction_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_Transaction_Test.log 2>&1
./Driver_Procedure_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_Procedure_Test.log 2>&1
./Driver_Dialect_Test > /home/leyden100/eclipse-workspace/OCI_Wrapper/logs/Driver_Dialect_Test.log 2>&1



