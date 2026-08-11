

#!/bin/bash

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

VERSION_DIR="$PROJECT_ROOT/version"
BUILD_NUMBER_FILE="$VERSION_DIR/build_number.txt"
MAJOR_NUMBER_FILE="$VERSION_DIR/major_number.txt"
MINOR_NUMBER_FILE="$VERSION_DIR/minor_number.txt"
OFFICIAL_VERSION_FILE="$VERSION_DIR/official_version.txt"

HEADER_FILE="$PROJECT_ROOT/include/build_number.h"

# Read version numbers
MAJOR_NUMBER=$(cat "$MAJOR_NUMBER_FILE")
MINOR_NUMBER=$(cat "$MINOR_NUMBER_FILE")
BUILD_NUMBER=$(cat "$BUILD_NUMBER_FILE")

# Increment build number
BUILD_NUMBER=$((BUILD_NUMBER + 1))

# Save new build number
echo "$BUILD_NUMBER" > "$BUILD_NUMBER_FILE"

# Create official version file
echo "Build Version : $MAJOR_NUMBER.$MINOR_NUMBER.$BUILD_NUMBER" > "$OFFICIAL_VERSION_FILE"

# Generate C/C++ build number header
cat > "$HEADER_FILE" <<EOF
#ifndef BUILD_NUMBER_H
#define BUILD_NUMBER_H

#define BUILD_NUMBER $BUILD_NUMBER
#define MAJOR_NUMBER $MAJOR_NUMBER
#define MINOR_NUMBER $MINOR_NUMBER

#endif
EOF

echo "Build Version : $MAJOR_NUMBER.$MINOR_NUMBER.$BUILD_NUMBER"


