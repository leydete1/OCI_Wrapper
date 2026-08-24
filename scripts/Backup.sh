cd ~/eclipse-workspace

rm -f *.gz


#Backup.sh
cd ~/eclipse-workspace

#Date/time pulled from the system at run time - no more manual editing
#of the timestamp before each backup. Edit LABEL below to change the
#descriptive part of the filename (e.g. what milestone this backup marks).
DATESTAMP=$(date +%Y_%m_%d_%H%M)
LABEL="http_consumer_complete_closed"

#Refresh packages list
dpkg --get-selections > /home/leyden100/eclipse-workspace/OCI_Wrapper/include/eclipse-workspaceinstalled-packages.txt

#New - lighter incremental backup, same content minus the oci/ directory
#(Oracle Instant Client binaries - large, unchanging, already captured in
#the full backup below, no need to duplicate them on every incremental run)
tar -czf OCI_Wrapper_backup_${DATESTAMP}_${LABEL}_light.tar.gz --exclude='OCI_Wrapper/oci' --exclude='OCI_Wrapper/Debug' --exclude='Backup' OCI_Wrapper


#Full backup
tar -czf OCI_Wrapper_backup_${DATESTAMP}_${LABEL}.tar.gz OCI_Wrapper


