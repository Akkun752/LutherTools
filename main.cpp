#include "mainwindow.h"
#include "version.h"

#include "third_party/logger.h"
#include <QApplication>
#include <QLocale>
#include <QTranslator>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);


    logger.info("Application start");

    // Identifie l'appli pour QSettings/QStandardPaths (fichier de préférences
    // dans %APPDATA%/Akkun7/LutherTools.ini, voir MainWindow::settingsFilePath()).
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

    logger.info("Application close");
    return QApplication::exec();
}
