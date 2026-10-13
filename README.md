# UltraPDF (PDF Читалка)

Простой просмотрщик PDF для Android: WebView-интерфейс + системный PdfRenderer.
Пакет `com.dd.pdfviewer`, minSdk 21, targetSdk 34.

Возможности: ленивая отрисовка страниц, щипок и двойной тап для масштаба, переход на страницу,
ночной режим, запоминание позиции, открытие последнего файла, открытие PDF из других приложений.

Готовый APK: `UltraPDF.apk` (подпись ключом dbg.jks, тем же, что у TITAN RAID).

Сборка: `SDK=/path/to/sdkmin KS=/path/to/dbg.jks ./build.sh <versionCode> <versionName> out.apk`
(aapt2 → javac → d8 → zipalign → apksigner). Внутренние нестатические классы в Java не использовать: d8 8.2 на них падает.
