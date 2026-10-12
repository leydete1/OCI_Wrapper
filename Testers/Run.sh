rm -f ./baseline/*.current      # fix-22: keep only this run's differences
./http_consumer_tester https://localhost:8443 \
  /home/leyden100/eclipse-workspace/OCI_Wrapper/test_data/consumers/file/Input_XML \
  /home/leyden100/eclipse-workspace/OCI_Wrapper/test_data/consumers/file/Input_JSON \
  8 ./baseline
