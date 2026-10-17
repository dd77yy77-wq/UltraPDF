#!/bin/bash
# Собирает dx (компилятор class -> dex) из исходников AOSP. Нужен только JDK. Результат: /tmp/dalvik/dxout
set -e
mkdir -p /tmp/dalvik && cd /tmp/dalvik
[ -d src ] || GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 --branch android-9.0.0_r1 https://github.com/aosp-mirror/platform_dalvik src
mkdir -p dxout && javac -nowarn -encoding UTF-8 -d dxout $(find src/dx/src -name '*.java')
echo "dx готов: /tmp/dalvik/dxout"
