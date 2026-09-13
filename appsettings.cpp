#include "appsettings.h"

#include <QDir>
#include <QStandardPaths>

QString appSettingsFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/preferences.ini");
}
