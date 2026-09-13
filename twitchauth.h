#ifndef TWITCHAUTH_H
#define TWITCHAUTH_H

#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Authentification Twitch via le "Device Code Grant Flow" (OAuth pensé pour
// les applications de bureau : pas de serveur local à faire tourner, pas de
// secret client à stocker — cf. dev.twitch.tv/docs/authentication).
//
// Déroulé :
//  1. startLogin() demande un device_code à Twitch, ouvre le navigateur sur
//     l'URL de vérification (le code y est pré-rempli) et se met à "poller"
//     le endpoint token en attendant que l'utilisateur autorise l'appli.
//  2. Une fois le token obtenu, validateToken() interroge /oauth2/validate
//     pour récupérer le pseudo (login) associé, puis fetchProfile() appelle
//     l'API Helix pour le nom d'affichage et l'avatar avant de confirmer
//     l'authentification.
//  3. Le token (+ refresh token) est sauvegardé localement ; au lancement
//     suivant, restoreSession() le relit et le rafraîchit si besoin, sans
//     repasser par le navigateur.
//
// Scope demandé : channel:manage:broadcast (lecture/modification du titre,
// de la catégorie et des tags du stream via l'API Helix /channels).
class TwitchAuth : public QObject
{
    Q_OBJECT

public:
    explicit TwitchAuth(QObject *parent = nullptr);
    ~TwitchAuth() override;

    // Tente de restaurer une session précédente (token sauvegardé). N'émet
    // rien si aucun token n'est enregistré ; sinon émet authenticated() ou
    // authFailed() selon que le token (éventuellement rafraîchi) est valide.
    void restoreSession();

    // Démarre le flux d'autorisation (ouvre le navigateur, poll le token).
    void startLogin();

    // Oublie la session (token local + en mémoire).
    void logout();

    bool isAuthenticated() const { return !m_login.isEmpty(); }
    QString login() const { return m_login; }
    QString userId() const { return m_userId; }
    QString accessToken() const { return m_accessToken; }

    // Les scopes réellement accordés (dépend de ce que l'utilisateur avait
    // autorisé au moment de sa connexion : une session plus ancienne peut ne
    // pas avoir les scopes demandés par une version plus récente de l'appli).
    bool hasScope(const QString &scope) const { return m_scopes.contains(scope); }

signals:
    // Le navigateur vient de s'ouvrir sur verificationUri ; userCode est
    // affiché en secours si jamais il faut le retaper à la main.
    void awaitingAuthorization(const QString &verificationUri, const QString &userCode);
    void authenticated(const QString &login, const QString &displayName, const QString &avatarUrl);
    void authFailed(const QString &reason);
    void loggedOut();

private:
    void requestDeviceCode();
    void pollToken();
    void handleTokenResponse(QNetworkReply *reply, bool isRefresh);
    void validateToken();
    void fetchProfile();
    void saveTokens();
    void clearTokens();

    QNetworkAccessManager *m_network;
    QTimer *m_pollTimer = nullptr;

    QString m_clientId;
    QString m_deviceCode;
    int m_pollIntervalSec = 5;

    QString m_accessToken;
    QString m_refreshToken;
    QString m_login;
    QString m_userId;
    QStringList m_scopes;
};

#endif // TWITCHAUTH_H
