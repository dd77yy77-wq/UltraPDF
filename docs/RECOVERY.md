# Восстановление проекта UltraPDF

Репозиторий: github.com/dd77yy77-wq/ultrapdf (публичный, ветка main). Рядом лежит приложение «Щиток» (`shchitok/`, у него свои docs).

## Что где
- Android-вьювер: `src/`, `res/`, `assets/` (WebView + pdf.js), `AndroidManifest.xml`, `build.sh`. Пакет `com.dd.pdfviewer`, minSdk 21, targetSdk 34. Релиз: `UltraPDF.apk` + `version.json` (код = версия без точки: 2.5 → 25).
- Windows-вьювер и редактор: `windows/` (C++17, Win32, PDFium внутри exe), `windows/TOOLCHAIN.md` (ссылки на компилятор, PDFium, Wine), тесты `windows/tests/`. Релизы: `UltraPDF.exe` + `version-win.json` (sha256 обязателен), `UltraPDF-Editor.exe` + `version-editor.json`.
- Тесты Android: `tests/` (README, раздел «Тесты»).

## Ключ подписи Android
- `keys/ultrapdf-dbg.jks.enc` (AES-256-CBC, pbkdf2, 200000 итераций). Внутри `dbg.jks`: alias `dbg`, пароль хранилища и ключа `android`. SHA-256 сертификата: `6E:65:4D:75:88:66:A8:3B:3B:E0:7D:69:8D:46:D7:8B:9E:30:DA:38:6A:E4:FB:67:3E:7D:CA:23:0D:12:B8:38`.
- Пароль расшифровки в репозитории и в памяти Claude не хранится: у пользователя на Google Drive, файл «UltraPDF — ключ подписи (пароль к зашифрованному ключу).txt».
- Расшифровка: `openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 -pass pass:ПАРОЛЬ -in keys/ultrapdf-dbg.jks.enc -out dbg.jks`.
- Подписывать только этим ключом, иначе телефон не обновит приложение.

## Как собрать
- Android: нужен Android SDK (`android-14` build-tools: aapt2, d8, zipalign, apksigner, core-lambda-stubs.jar; `android-34/android.jar`). Из песочницы его скачать нельзя (dl.google.com закрыт); архив `android-sdk-min` лежит у пользователя на компьютере (папка Claude games). `SDK=/путь/sdkmin KS=/путь/dbg.jks ./build.sh <код> <имя> out.apk`. Внутренние нестатические классы Java не использовать (d8 падает).
- Windows: см. `windows/TOOLCHAIN.md`; `cd windows && TC=… PDFIUM_INC=… ./build.sh`; `tests/run.sh tN.txt <сек>` (Wine + Xvfb, переменная `ULTRAPDF_TEST`).

## Проверки перед выпуском
- Android: `apksigner verify --print-certs` (отпечаток как выше), `zipalign -c -v 4`, новые versionCode/versionName, `version.json` обновлён.
- Windows: свой тест под Wine, sha256 в `version-win.json` совпадает с `UltraPDF.exe` в корне.

## Правила пользователя
Русский, коротко, с честной пометкой, что на его ПК/телефоне не запускалось. Всегда готовый файл (APK/exe через SendUserFile и публикация на GitHub), без инструкций. Сначала пробовать и проверять самому. Все исходники сохранять на GitHub после каждой сборки.
