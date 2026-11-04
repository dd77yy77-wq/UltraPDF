# Чем собирается и проверяется Windows-версия (всё из публичных релизов GitHub)

Сборка идёт на Linux кросс-компилятором, ничего на Windows ставить не нужно.

| Что | Откуда |
|---|---|
| llvm-mingw 20250924 (ucrt, x86_64) | https://github.com/mstorsjo/llvm-mingw/releases/download/20250924/llvm-mingw-20250924-ucrt-ubuntu-22.04-x86_64.tar.xz |
| PDFium 157.0.8100.0 (win-x64: include/*.h и bin/pdfium.dll) | https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F8100/pdfium-win-x64.tgz |
| Wine 10.0 (проверка без Windows) | https://github.com/Kron4ek/Wine-Builds/releases/download/10.0/wine-10.0-amd64.tar.xz |

Порядок: распаковать llvm-mingw и pdfium (например в /tmp/wl), положить `pdfium/bin/pdfium.dll` в `windows/res/pdfium.dll`, затем
`cd windows && TC=/tmp/wl/llvm-mingw-20250924-ucrt-ubuntu-22.04-x86_64 PDFIUM_INC=/tmp/wl/pdfium/include ./build.sh`.
Тесты: `tests/run.sh tN.txt <сек>` (Wine + Xvfb, переменная `ULTRAPDF_TEST`). Для Unicode-имён файлов нужен `LANG=C.UTF-8`.

Android: Android SDK (build-tools 14, android-34) и ключ подписи `dbg.jks` лежат у владельца и в архиве `android-sdk-min`; ключ должен быть тем же, иначе телефон не обновит приложение.
