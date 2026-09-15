#ifndef TWITCHAUTH_H
#define TWITCHAUTH_H

#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Twitch authentication via the "Device Code Grant Flow" (OAuth designed for
// desktop applications: no local server to run, no client secret to store —
// see dev.twitch.tv/docs/authentication).
//
// Flow:
//  1. startLogin() requests a device_code from Twitch, opens the browser on
//     the verification URL (the code is pre-filled in) and starts "polling"
//     the token endpoint while waiting for the user to authorize the app.
//  2. Once the token is obtained, validateToken() queries /oauth2/validate
//     to get the associated username (login), then fetchProfile() calls the
//     Helix API for the display name and avatar before confirming
//     authentication.
//  3. The token (+ refresh token) is saved locally; on the next launch,
//     restoreSession() reads it back and refreshes it if needed, without
//     going through the browser again.
//
// Scope requested: channel:manage:broadcast (reading/editing the stream's
// title, category and tags via the Helix /channels API).
class TwitchAuth : public QObject
{
    Q_OBJECT

public:
    explicit TwitchAuth(QObject *parent = nullptr);
    ~TwitchAuth() override;

    // Attempts to restore a previous session (saved token). Emits nothing
    // if no token is stored; otherwise emits authenticated() or
    // authFailed() depending on whether the (possibly refreshed) token is
    // valid.
    void restoreSession();

    // Starts the authorization flow (opens the browser, polls the token).
    void startLogin();

    // Forgets the session (local token + in-memory).
    void logout();

    bool isAuthenticated() const { return !m_login.isEmpty(); }
    QString login() const { return m_login; }
    QString userId() const { return m_userId; }
    QString accessToken() const { return m_accessToken; }

    // The scopes actually granted (depends on what the user authorized at
    // the time they logged in: an older session might not have the scopes
    // requested by a more recent version of the app).
    bool hasScope(const QString &scope) const { return m_scopes.contains(scope); }

signals:
    // The browser has just opened on verificationUri; userCode is shown as
    // a fallback in case it needs to be typed in by hand.
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
