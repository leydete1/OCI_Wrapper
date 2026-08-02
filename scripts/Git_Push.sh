
#!/bin/bash

cd ~/eclipse-workspace/OCI_Wrapper || exit 1

git add .
git reset wallet/

if ! git diff --cached --quiet; then
    git commit -m "Automated backup $(date '+%Y-%m-%d %H:%M:%S')"
    git push
fi


