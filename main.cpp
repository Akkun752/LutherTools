#include "mainwindow.h"
#include "version.h"
#include "third_party/logger.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // The global `logger` (declared in logger.h, defined in logger.cpp) is
    // used here and throughout the app (Chat, TwitchAuth, MainWindow...) so
    // every event of a run lands in the same log file. A local `Logger
    // logger;` here would just shadow it within main() and write to a
    // second, separate file instead.
    logger.info("Application start");
    // Identifies the app for QSettings/QStandardPaths (preferences file in
    // %APPDATA%/FlitStudio/LutherTools.ini, see appSettingsFilePath()).
    QCoreApplication::setOrganizationName(QStringLiteral("FlitStudio"));
    QCoreApplication::setApplicationName(QStringLiteral("LutherTools"));
    QCoreApplication::setApplicationVersion(QStringLiteral(LUTHERTOOLS_VERSION_STRING));

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = "LutherTools_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            a.installTranslator(&translator);
            break;
        }
    }
    MainWindow w;
    w.show();
    return QApplication::exec();
}
