#!/bin/bash
# usage: SDK=/tmp/sdkmin ./build.sh <versionCode> <versionName> <out.apk>
set -e
SDK=${SDK:-/tmp/sdk}; BT=$SDK/android-14; AJ=$SDK/android-34/android.jar
export PATH=$BT:$PATH
cd "$(dirname "$0")"
B=build; rm -rf $B "$3"; mkdir -p $B/classes $B/dex
aapt2 compile --dir res -o $B/res.zip
aapt2 link --auto-add-overlay -o $B/base.apk -I $AJ -R $B/res.zip --manifest AndroidManifest.xml \
  --min-sdk-version 21 --target-sdk-version 34 --version-code $1 --version-name $2 -A assets
javac -encoding UTF-8 -source 8 -target 8 -Xlint:-options -g:none -bootclasspath $AJ:$BT/core-lambda-stubs.jar \
  -d $B/classes $(find src -name '*.java')
d8 --lib $AJ --min-api 21 --output $B/dex $(find $B/classes -name '*.class')
cp $B/base.apk $B/unsigned.apk
zip -q -j $B/unsigned.apk $B/dex/classes.dex
zipalign -f -p 4 $B/unsigned.apk $B/aligned.apk
apksigner sign --ks ${KS:-/tmp/sdkmin/dbg.jks} --ks-pass pass:android --key-pass pass:android --out "$3" $B/aligned.apk
echo built "$3"
