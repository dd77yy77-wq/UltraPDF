#!/bin/bash
# Публикация релиза Щитка: сохраняет ВСЕ исходники + APK в GitHub. Запуск: tools/publish.sh <code> <name> "<заметки>"
# Рабочая копия: /home/claude/shchitok, репозиторий: /home/claude/ultrapdf (папка shchitok/)
set -e
W=/home/claude/shchitok; R=/home/claude/ultrapdf/shchitok
for d in assets src stubs tools; do rm -rf $R/$d; cp -a $W/$d $R/$d; done; cp $W/build.py $R/build.py; find $R -name __pycache__ -prune -exec rm -rf {} +
cp /home/claude/out/Shchitok.apk $R/Shchitok.apk
printf '{"code": %s, "name": "%s", "notes": "%s"}\n' "$1" "$2" "$3" > $R/version.json
cd /home/claude/ultrapdf && git add -A shchitok && git commit -q -m "Щиток $2: $3" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01CaPh1BHmUR1XCMVYytpHi3" && git push 2>&1 | grep -v moved | grep -v remote
