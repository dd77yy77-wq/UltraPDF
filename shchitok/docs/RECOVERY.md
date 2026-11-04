# Восстановление проекта «Щиток»

Пакет `com.dd.shchitok`, minSdk 24, targetSdk 34. Интерфейс — `assets/index.html` (WebView).

## Где что лежит
- Репозиторий: github.com/dd77yy77-wq/UltraPDF, папка `shchitok/` (публичный репозиторий).
- Исходники: `assets/`, `src/`, `stubs/`, `tools/`, `build.py`. Исходные фото: `sources/`. Тесты: `tests/`.
- Релиз для автообновления: `Shchitok.apk` + `version.json` в этой папке (ветка main).
- Ключ подписи: `keys/shchitok.p12.enc` (AES-256, зашифрован). Хранилище внутри: alias `shchitok`, пароль хранилища `android`.
  SHA-256 сертификата: `08:74:8C:8D:24:64:69:F8:5D:45:B8:41:62:30:FF:2D:DA:B1:FB:9C:78:64:27:06:4E:14:8E:0F:43:F8:04:69`.
  Пароль расшифровки в репозитории и в памяти Claude НЕ хранится: он у пользователя (файл на Google Drive «Щиток — ключ подписи.txt»).
  Расшифровка: `openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 -pass pass:ПАРОЛЬ -in keys/shchitok.p12.enc -out /home/claude/keys/shchitok.p12`

## Как собрать
1. Нужны JDK и Python 3 с `Pillow`, `cryptography`, `rsync` не нужен.
2. `tools/setup_dx.sh` (собирает dx из AOSP в /tmp/dalvik/dxout; DXCP=/tmp/dalvik/dxout).
3. Ключ положить в `/home/claude/keys/shchitok.p12` (или задать `KEYDIR`).
4. `python3 build.py <versionCode> <versionName> /home/claude/out/Shchitok.apk`. Без лямбд (только анонимные классы).
5. Проверки: `testzip`, `apkbuild.v2_verify`, `jarsigner -verify`, тест Playwright (tests/sch_test.py) в окне 412×860.
6. Публикация: `tools/publish.sh <code> <name> "<заметки>"` (копирует исходники и APK в репозиторий, коммит, push), затем SendUserFile с APK.

## Правила пользователя
Русский, коротко; всегда готовый подписанный APK, а не инструкции; подписывать ТОЛЬКО этим ключом (иначе обновление не встанет); сохранять все исходники на GitHub после каждой сборки.

## Ключевые факты о коде
- Панели: left/right, SVG viewBox 2576×1840; схема по умолчанию, повторное нажатие на вкладку переключает схема ⇄ фото.
- Мост `AndroidApp`: version(), checkUpdate(), shareApk(), dial(String). Автообновление читает `shchitok/version.json` с raw.githubusercontent.com.
- Подписи автоматов: `LDEF`/`RDEF` в index.html (пустые = «Не используется»). Свободны: левый 8–12; правый 4, 5, 7, 9, 11–14, 16.
