
#!/bin/bash

cd ~/eclipse-workspace/OCI_Wrapper || exit 1

git add .

if ! git diff --cached --quiet; then
    git commit -m "Automated backup $(date '+%Y-%m-%d %H:%M:%S')"
    git push
fi

#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

