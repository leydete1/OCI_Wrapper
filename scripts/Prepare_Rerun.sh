cd /home/leyden100/eclipse-workspace/OCI_Wrapper/logs

rm -f *.log
rm -f *.csv



cd /home/leyden100/eclipse-workspace/OCI_Wrapper/output_clobs
rm -f *.txt

cd /home/leyden100/eclipse-workspace/OCI_Wrapper/output_lobs
rm -f *.jpg


cd /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Error_JSON
rm -f *
cd /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Error_XML
rm -f *

cd /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Input_JSON
rm -f *
cd /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Input_XML
rm -f *


cd /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Output_JSON
rm -f *
cd /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Output_XML
rm -f *

cd /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Processing_JSON
rm -f *
cd /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Processing_XML
rm -f *

cp -f /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Input_JSON_templates/* /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Input_JSON
cp -f /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Input_XML_Templates/* /home/leyden100/eclipse-workspace/OCI_Wrapper/consumers/file/Input_XML



