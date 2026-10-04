# This script requires create-dmg to be installed from https://github.com/sindresorhus/create-dmg
BUILD_CONFIG=$1

fail()
{
	echo "$1" 1>&2
	exit 1
}

if [ "$BUILD_CONFIG" != "Debug" ] && [ "$BUILD_CONFIG" != "Release" ]; then
  fail "Invalid build configuration - expected 'Debug' or 'Release'"
fi

BUILD_ROOT=$PWD/build
SOURCE_ROOT=$PWD
BUILD_FOLDER=$BUILD_ROOT/build-$BUILD_CONFIG
INSTALLER_FOLDER=$BUILD_ROOT/installer-$BUILD_CONFIG

if [ -n "$CI_VERSION" ]; then
  VERSION=$CI_VERSION
else
  VERSION=`cat $SOURCE_ROOT/app/version.txt`
fi

if [ "$SIGNING_PROVIDER_SHORTNAME" == "" ]; then
  SIGNING_PROVIDER_SHORTNAME=$SIGNING_IDENTITY
fi
if [ "$SIGNING_IDENTITY" == "" ]; then
  SIGNING_IDENTITY=$SIGNING_PROVIDER_SHORTNAME
fi

[ "$SIGNING_IDENTITY" == "" ] || git diff-index --quiet HEAD -- || fail "Signed release builds must not have unstaged changes!"

echo Updating dependencies
python3 $SOURCE_ROOT/setup-deps.py

echo Cleaning output directories
rm -rf $BUILD_FOLDER
rm -rf $INSTALLER_FOLDER
mkdir $BUILD_ROOT
mkdir $BUILD_FOLDER
mkdir $INSTALLER_FOLDER

# Enable LTO for official builds
export CFLAGS=-flto=thin
export CXXFLAGS=-flto=thin
export LDFLAGS=-flto=thin

echo Configuring the project
pushd $BUILD_FOLDER
qmake $SOURCE_ROOT/moonlight-qt.pro QMAKE_APPLE_DEVICE_ARCHS="x86_64 arm64" || fail "Qmake failed!"
popd

echo Compiling Moonlight in $BUILD_CONFIG configuration
pushd $BUILD_FOLDER
make -j$(sysctl -n hw.logicalcpu) $(echo "$BUILD_CONFIG" | tr '[:upper:]' '[:lower:]') || fail "Make failed!"
popd

echo Saving dSYM file
pushd $BUILD_FOLDER
dsymutil app/MoonlightVibe.app/Contents/MacOS/MoonlightVibe -o MoonlightVibe-$VERSION.dsym || fail "dSYM creation failed!"
cp -R MoonlightVibe-$VERSION.dsym $INSTALLER_FOLDER || fail "dSYM copy failed!"
popd

echo Creating app bundle
EXTRA_ARGS=
if [ "$BUILD_CONFIG" == "Debug" ]; then EXTRA_ARGS="$EXTRA_ARGS -use-debug-libs"; fi
echo Extra deployment arguments: $EXTRA_ARGS
macdeployqt $BUILD_FOLDER/app/MoonlightVibe.app $EXTRA_ARGS -qmldir=$SOURCE_ROOT/app/gui -appstore-compliant || fail "macdeployqt failed!"

echo Removing dSYM files from app bundle
find $BUILD_FOLDER/app/MoonlightVibe.app/ -name '*.dSYM' | xargs rm -rf

if [ "$SIGNING_IDENTITY" != "" ]; then
  echo Signing app bundle with Developer ID identity
  codesign --force --deep --options runtime --timestamp --sign "$SIGNING_IDENTITY" $BUILD_FOLDER/app/MoonlightVibe.app || fail "Signing failed!"
else
  echo No signing identity configured - ad-hoc signing app bundle instead
  codesign --force --deep --sign - $BUILD_FOLDER/app/MoonlightVibe.app || fail "Ad-hoc signing failed!"
fi

echo Creating DMG
if [ "$SIGNING_IDENTITY" != "" ]; then
  create-dmg $BUILD_FOLDER/app/MoonlightVibe.app $INSTALLER_FOLDER --identity="$SIGNING_IDENTITY" --no-version-in-filename || fail "create-dmg failed!"
else
  # The app bundle above was ad-hoc signed (no certificate), not signed with a real Developer ID
  # identity. create-dmg's --identity flag signs the .dmg container itself and expects a real
  # signing identity, which ad-hoc signing cannot provide, so we deliberately do not pass it here.
  # This is believed correct: Gatekeeper's "damaged and can't be opened" check runs against the
  # app bundle's own signature at launch time, not the DMG container's signature, so ad-hoc
  # signing the .app above is what fixes that failure mode. Leaving the DMG itself unsigned in
  # this branch has not been verified end-to-end on a real Mac.
  create-dmg $BUILD_FOLDER/app/MoonlightVibe.app $INSTALLER_FOLDER --no-version-in-filename
  case $? in
    0) ;;
    2) ;;
    *) fail "create-dmg failed!";;
  esac
fi

if [ "$NOTARY_KEYCHAIN_PROFILE" != "" ]; then
  echo Uploading to App Notary service
  xcrun notarytool submit --keychain-profile "$NOTARY_KEYCHAIN_PROFILE" --wait $INSTALLER_FOLDER/MoonlightVibe.dmg || fail "Notary submission failed"

  echo Stapling notary ticket to DMG
  xcrun stapler staple -v $INSTALLER_FOLDER/MoonlightVibe.dmg || fail "Notary ticket stapling failed!"
fi

mv $INSTALLER_FOLDER/MoonlightVibe.dmg $INSTALLER_FOLDER/Moonlight-$VERSION.dmg
echo Build successful