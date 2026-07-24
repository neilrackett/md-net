#!/bin/bash
# Abort on the first failing step: without this, a failed sub-build (e.g.
# the RP compile) was survived silently -- the script then copied a STALE
# rp/dist UF2 into dist/ and reported success, shipping old firmware
# under a new version number.
set -e

# Get the absolute path of the current script
SCRIPT_DIR=$(dirname "$(realpath "$0")")

# Ensure all required arguments are provided
if [ -z "$1" ] || [ -z "$2" ] || [ -z "$3" ]; then
    echo "Usage: $0 <board_type> <build_type> <app_uuid_key>"
    echo "Example: $0 pico|pico_w debug|release 123e4567-e89b-12d3-a456-426614174000"
    exit 1
fi

# Increment patch version in root version.txt on every build run and sync sub-project files
"$SCRIPT_DIR/tools/bump_version.sh" --repo-root "$SCRIPT_DIR"

# Display the version information
export VERSION=$(cat version.txt)
echo "Version: $VERSION"

# Set the board type to be used for building
export BOARD_TYPE=$1
echo "Board type: $BOARD_TYPE"

# Set the release or debug build type (normalized to lowercase so callers
# may pass "Release"/"Debug" -- matches rp/build.sh's own normalization).
export BUILD_TYPE=$(echo "$2" | tr '[:upper:]' '[:lower:]')
echo "Build type: $BUILD_TYPE"

# Set the APP_UUID_KEY of the app to be built
export APP_UUID_KEY=$3
echo "App UUID Key: $APP_UUID_KEY"

# Set the dist directory. Delete previous contents if any
echo "Delete previous dist directory"
rm -rf dist
mkdir dist

# Build the project in the target architecture
echo "Building target project"
cd target/atarist
./build.sh "$SCRIPT_DIR/target/atarist" release
cd ../..
echo "Done building target project"

# Build the STinG driver (MDNET.STX) with the m68k-atari-mint GCC cross
# toolchain from the same atarist-toolkit Docker image. Ships in dist/
# next to the UF2 -- the user copies it to the ST's STING folder.
echo "Building MDNET.STX (STinG driver)"
docker run --rm -v "$SCRIPT_DIR/target/atarist/stx:/work" -w /work \
    --entrypoint sh "${STCMD_IMAGE:-neilrackett/atarist-toolkit-docker-arm64:1.2.1}" \
    -c 'make clean >/dev/null && make'
if [ ! -f target/atarist/stx/MDNET.STX ]; then
    echo "ERROR: MDNET.STX not produced. Aborting."
    exit 1
fi
cp target/atarist/stx/MDNET.STX dist/MDNET.STX
cp target/atarist/stx/README.TXT dist/README.TXT
echo "Done building MDNET.STX"

# Build the rp project in the RP architecture
echo "Building rp project"
cd rp
./build.sh "$BOARD_TYPE" "$BUILD_TYPE"
if [ "$BUILD_TYPE" = "release" ]; then
    cp  ./dist/rp-$BOARD_TYPE.uf2 ../dist/rp.uf2
else
    cp  ./dist/rp-$BOARD_TYPE-$BUILD_TYPE.uf2 ../dist/rp.uf2
fi
cd ..
echo "Done building rp project"

# Fail loudly if the RP firmware wasn't produced, instead of continuing
# and leaving an empty dist/ for downstream steps (e.g. the CI release job,
# which then fails on a confusing `cp dist/*.uf2`).
if [ ! -f dist/rp.uf2 ]; then
    echo "ERROR: RP firmware not produced (dist/rp.uf2 missing). Aborting."
    exit 1
fi

# Calculate the md5sum of the generated rp.uf2 file
md5sum dist/rp.uf2 > dist/rp.uf2.md5sum

# Show the md5sum of the generated rp.uf2 file
echo "md5sum of the generated rp.uf2 file:"
cat dist/rp.uf2.md5sum

# Now inform the user that the build is complet and must
# modify the app.json file with the new md5sum and the UUID
echo "Build completed successfully. Please update the app.json file with the new md5sum and the UUID"

# Rename the file to the standard name <APP_UUID>.uf2
mv dist/rp.uf2 dist/$APP_UUID_KEY.uf2

# Check that there is a app.json file in the dist directory
if [ ! -f desc/app.json ]; then
    echo "app.json file not found in the 'desc'' directory. Please create one."
    exit 1
fi

# Copy the app.json file to the dist directory
cp desc/app.json dist/

# Use portable sed for Linux and macOS
if [ "$(uname)" = "Darwin" ]; then
    sed -i '' "s/<APP_UUID>/$APP_UUID_KEY/g" dist/app.json
    sed -i '' "s/<BINARY_MD5_HASH>/$(cat dist/rp.uf2.md5sum | cut -d ' ' -f 1)/g" dist/app.json
    sed -i '' "s/<APP_VERSION>/$VERSION/g" dist/app.json
else
    sed -i "s/<APP_UUID>/$APP_UUID_KEY/g" dist/app.json
    sed -i "s/<BINARY_MD5_HASH>/$(cat dist/rp.uf2.md5sum | cut -d ' ' -f 1)/g" dist/app.json
    sed -i "s/<APP_VERSION>/$VERSION/g" dist/app.json
fi

mv dist/$APP_UUID_KEY.uf2 dist/$APP_UUID_KEY-$VERSION.uf2

# Show the content of the $APP_UUID_KEY.json file
echo "Content of the $APP_UUID_KEY.json file:"
mv dist/app.json dist/$APP_UUID_KEY.json
cat dist/$APP_UUID_KEY.json

# Done
exit 0
