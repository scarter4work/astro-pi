#!/bin/bash
# NukeX Release Script
# Usage: ./scripts/release.sh [major|minor|patch]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

# Read current version
CURRENT_VERSION=$(cat VERSION)
IFS='.' read -r MAJOR MINOR PATCH <<< "$CURRENT_VERSION"

# Determine version bump type
BUMP_TYPE="${1:-patch}"

case "$BUMP_TYPE" in
    major)
        MAJOR=$((MAJOR + 1))
        MINOR=0
        PATCH=0
        ;;
    minor)
        MINOR=$((MINOR + 1))
        PATCH=0
        ;;
    patch)
        PATCH=$((PATCH + 1))
        ;;
    *)
        echo "Usage: $0 [major|minor|patch]"
        exit 1
        ;;
esac

NEW_VERSION="${MAJOR}.${MINOR}.${PATCH}"
echo "Bumping version: $CURRENT_VERSION -> $NEW_VERSION"

# Update VERSION file
echo "$NEW_VERSION" > VERSION

# Update NukeXModule.cpp version
sed -i "s/#define MODULE_VERSION_MAJOR.*/#define MODULE_VERSION_MAJOR     $MAJOR/" src/NukeXModule.cpp
sed -i "s/#define MODULE_VERSION_MINOR.*/#define MODULE_VERSION_MINOR     $MINOR/" src/NukeXModule.cpp
sed -i "s/#define MODULE_VERSION_REVISION.*/#define MODULE_VERSION_REVISION  $PATCH/" src/NukeXModule.cpp

# Update release date
YEAR=$(date +%Y)
MONTH=$(date +%-m)
DAY=$(date +%-d)
sed -i "s/#define MODULE_RELEASE_YEAR.*/#define MODULE_RELEASE_YEAR      $YEAR/" src/NukeXModule.cpp
sed -i "s/#define MODULE_RELEASE_MONTH.*/#define MODULE_RELEASE_MONTH     $MONTH/" src/NukeXModule.cpp
sed -i "s/#define MODULE_RELEASE_DAY.*/#define MODULE_RELEASE_DAY       $DAY/" src/NukeXModule.cpp

echo "Building..."
make clean
make -j8

echo "Signing module..."
make sign

echo "Creating package..."
RELEASE_DATE=$(date +%Y%m%d)
PACKAGE_NAME="${RELEASE_DATE}-linux-x64-NukeX.tar.gz"
tar -czvf "repository/$PACKAGE_NAME" NukeX-pxm.so NukeX-pxm.xsgn

# Get SHA1
PACKAGE_SHA1=$(sha1sum "repository/$PACKAGE_NAME" | cut -d' ' -f1)

echo "Updating updates.xri..."
cat > repository/updates.xri << EOF
<?xml version="1.0" encoding="UTF-8"?>
<xri version="1.0" xmlns="http://www.pixinsight.com/xri" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:schemaLocation="http://www.pixinsight.com/xri http://pixinsight.com/xri/xri-1.0.xsd">
   <description>
      <p>
         <b>NukeX Update Repository</b>
      </p>
      <p>
         Intelligent region-aware processing for PixInsight by Scott Carter.
      </p>
   </description>
   <platform os="linux" arch="x64" version="1.8.0:1.9.9">
      <package fileName="$PACKAGE_NAME" sha1="$PACKAGE_SHA1" type="module" releaseDate="$RELEASE_DATE">
         <title>NukeX ${NEW_VERSION} - Intelligent Astrophotography Processing</title>
         <description>
            <p>
               <b>NukeX - Intelligent Region-Aware Processing for PixInsight</b>
            </p>
            <p>
               NukeX provides two powerful processes for astrophotography:
            </p>
            <ul>
               <li>
                  <b>NukeX</b> - Region-aware image stretching using AI segmentation.
               </li>
               <li>
                  <b>NukeXStack</b> - Intelligent pixel selection for subframe integration.
               </li>
            </ul>
            <p>
               Copyright (c) 2026 Scott Carter. All Rights Reserved.
            </p>
         </description>
      </package>
   </platform>
</xri>
EOF

echo "Signing updates.xri..."
/opt/PixInsight/bin/PixInsight.sh \
  --sign-xml-file=repository/updates.xri \
  --xssk-file=/home/scarter4work/projects/keys/scarter4work_keys.xssk \
  --xssk-password="***REDACTED***"

echo "Committing changes..."
git add -A
git commit -m "Release v${NEW_VERSION}

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
git push

echo "Creating GitHub release..."
gh release create "v${NEW_VERSION}" \
  "repository/$PACKAGE_NAME" \
  repository/updates.xri \
  --title "NukeX ${NEW_VERSION}" \
  --notes "NukeX ${NEW_VERSION} release

To install via PixInsight updater, add this repository URL:
\`https://raw.githubusercontent.com/scarter4work/NukeX2/main/repository/\`"

echo ""
echo "========================================="
echo "Release v${NEW_VERSION} complete!"
echo "========================================="
echo ""
echo "Repository URL for PixInsight:"
echo "https://raw.githubusercontent.com/scarter4work/NukeX2/main/repository/"
