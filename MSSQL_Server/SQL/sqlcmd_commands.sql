sqlcmd -S localhost -U SA -C -d DataManager -Q "GRANT SELECT, INSERT, UPDATE, DELETE, EXECUTE ON SCHEMA::DATA_MANAGER TO DATA_MANAGER;"
sqlcmd -S localhost -U SA -C -d DataManager -Q "GRANT VIEW DEFINITION ON SCHEMA::DATA_MANAGER TO DATA_MANAGER;"
sqlcmd -S localhost -U SA -C -d DataManager -Q "GRANT ALTER ON DATA_MANAGER.OCI_FIELD_TEST TO DATA_MANAGER;"

sqlcmd -S localhost -U SA -C -d DataManager -Q "select count(*) as rec_count from oci_field_test ;"
