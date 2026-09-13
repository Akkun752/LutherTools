#ifndef APPSETTINGS_H
#define APPSETTINGS_H

#include <QString>

// Chemin du fichier local de préférences (créé si besoin), partagé par
// MainWindow (réglages UI) et TwitchAuth (tokens).
QString appSettingsFilePath();

#endif // APPSETTINGS_H
