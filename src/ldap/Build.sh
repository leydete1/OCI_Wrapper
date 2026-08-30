# Build.sh snippet - places the compiled binary directly beside
# OCI_Wrapper's own output, which is exactly where
# ldap_auth_helper.c's find_helper_path() expects to find it at
# runtime (it resolves /proc/self/exe's own directory and looks
# for "ldap_bind_helper" right next to it - no path config needed):
gcc -O2 -Wall -o ../../Debug/ldap_bind_helper ./ldap_bind_helper.c -lldap
#cp -f ../../Debug/ldap_bind_helper ../../
chmod +x ../../Debug/ldap_bind_helper
#ensure docker ldap is running
#docker start openldap-test
