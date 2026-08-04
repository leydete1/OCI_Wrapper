cd /home/leyden100/eclipse-workspace/OCI_Wrapper/logs

rm -f *.log
rm -f *.csv



cd /home/leyden100/eclipse-workspace/OCI_Wrapper/output_clobs
rm -f *.txt

cd /home/leyden100/eclipse-workspace/OCI_Wrapper/output_lobs
rm -f *.jpg

cp /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Processing_JSON/*.* /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Input_JSON
cp /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Processing_XML/*.* /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Input_XML



rm -f /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Processing_XML/*.*
rm -f /leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Processing_JSON/*.*

