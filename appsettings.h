#ifndef APPSETTINGS_H
#define APPSETTINGS_H

#include <QString>

// Path of the local preferences file (created if needed), shared by
// MainWindow (UI settings) and TwitchAuth (tokens).
QString appSettingsFilePath();

#endif // APPSETTINGS_H
