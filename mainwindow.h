#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QList>
#include <QMainWindow>
#include <QMap>
#include <QPair>
#include <QStringList>
#include <QVector>

class Chat;
class TTS;
class TwitchAuth;
class TwitchChannel;
class QNetworkAccessManager;
class QCompleter;
class QStringListModel;
class QTimer;
class FlowLayout;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

// Un préset de configuration de stream (titre/catégorie/tags), enregistré
// localement pour pouvoir y re-switcher rapidement.
struct StreamPreset
{
    QString name;
    QString title;
    QString gameName;
    QString gameId;
    QStringList tags;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onChatMessageReceived(const QString &username, const QString &message);
    void onChatStatusChanged(const QString &status);
    void onChatSendButtonClicked();
    void onTwitchLoginButtonClicked();
    void onTwitchAuthenticated(const QString &login, const QString &displayName, const QString &avatarUrl);
    void onTwitchAuthFailed(const QString &reason);
    void onTwitchLoggedOut();
    void onStreamInfoReceived(const QString &title, const QString &gameName, const QString &gameId,
                               const QStringList &tags);
    void onStreamInfoFailed(const QString &reason);
    void onStreamUpdateSucceeded();
    void onStreamUpdateFailed(const QString &reason);
    void onStreamFetchButtonClicked();
    void onStreamSaveButtonClicked();
    void onStreamCategoryTextEdited(const QString &text);
    void onCategoriesFound(const QVector<QPair<QString, QString>> &categories);
    void onStreamTagInputReturnPressed();
    void onPresetSaveButtonClicked();

private:
    void attachChat(Chat *chat);
    void loadSettings();
    void saveSettings();
    void setStreamInfoEnabled(bool enabled);
    void addTag(const QString &tag);
    void removeTag(const QString &tag);
    void rebuildTagChips();
    void loadPresets();
    void savePresets();
    void refreshPresetList();
    void loadPreset(const QString &name);
    void deletePreset(const QString &name);
    void showPresetInfoDialog(const QString &name);
    int presetIndexByName(const QString &name) const;
    QString resolveCategoryId(const QString &categoryText) const;

    Ui::MainWindow *ui;
    Chat *m_chat = nullptr;
    TTS *m_tts = nullptr;
    TwitchAuth *m_twitchAuth = nullptr;
    TwitchChannel *m_twitchChannel = nullptr;
    QNetworkAccessManager *m_network = nullptr;

    QCompleter *m_categoryCompleter = nullptr;
    QStringListModel *m_categoryModel = nullptr;
    QTimer *m_categorySearchTimer = nullptr;
    QMap<QString, QString> m_categoryIdByName;
    QString m_currentGameId;

    FlowLayout *m_tagsFlowLayout = nullptr;
    QStringList m_currentTags;

    static constexpr int kMaxPresets = 10;
    QList<StreamPreset> m_presets;
};
#endif // MAINWINDOW_H
