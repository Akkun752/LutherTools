#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "appsettings.h"
#include "chat.h"
#include "flowlayout.h"
#include "third_party/logger.h"
#include "tts.h"
#include "twitchauth.h"
#include "twitchchannel.h"
#include "version.h"

#include <QCloseEvent>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStringListModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    setWindowTitle(QStringLiteral("LutherTools v%1").arg(QStringLiteral(LUTHERTOOLS_VERSION_STRING)));

    // Permanent branding message at the bottom of the window. A "permanent"
    // widget (rather than showMessage()) stays displayed even when a
    // transient message (e.g. a failed Twitch login) appears next to it.
    auto *versionLabel = new QLabel(
        QStringLiteral("LutherTools v%1 - Flit Studio").arg(QStringLiteral(LUTHERTOOLS_VERSION_STRING)), this);
    ui->statusbar->addPermanentWidget(versionLabel);

    // "About" menu: purely informative entry (not connected to anything -
    // clicking it does nothing), just showing the current version at a
    // glance without opening the full About dialog.
    ui->actionVersion->setText(tr("Version v%1").arg(QStringLiteral(LUTHERTOOLS_VERSION_STRING)));

    // Connection / Presets / Chat in strictly equal thirds: same stretch
    // factor for all 3, and an explicit split once the window actually has
    // its real size (before show(), the splitter doesn't have its final
    // width yet and just falls back to each pane's preferred size, which
    // favors whichever has the most controls).
    ui->mainSplitter->setStretchFactor(0, 1);
    ui->mainSplitter->setStretchFactor(1, 1);
    ui->mainSplitter->setStretchFactor(2, 1);
    QTimer::singleShot(0, this, [this]() {
        const int third = ui->mainSplitter->width() / 3;
        ui->mainSplitter->setSizes({third, third, third});
    });

    m_network = new QNetworkAccessManager(this);
    m_tts = new TTS(this);

    // "Read 1 message out of N" filter: the checkbox enables/disables the field.
    connect(ui->readEveryNCheckBox, &QCheckBox::toggled, ui->readEveryNSpinBox, &QSpinBox::setEnabled);
    connect(ui->readEveryNCheckBox, &QCheckBox::toggled, m_tts, &TTS::setReadEveryNEnabled);
    connect(ui->readEveryNSpinBox, &QSpinBox::valueChanged, m_tts, &TTS::setReadEveryN);

    // "Random saturation" filter: the checkbox enables/disables both fields.
    connect(ui->saturationCheckBox, &QCheckBox::toggled, ui->saturationFactorSpinBox, &QSpinBox::setEnabled);
    connect(ui->saturationCheckBox, &QCheckBox::toggled, ui->saturationChanceSpinBox, &QSpinBox::setEnabled);
    connect(ui->saturationCheckBox, &QCheckBox::toggled, m_tts, &TTS::setSaturationEnabled);
    connect(ui->saturationFactorSpinBox, &QSpinBox::valueChanged, m_tts, &TTS::setSaturationFactor);
    connect(ui->saturationChanceSpinBox, &QSpinBox::valueChanged, m_tts, &TTS::setSaturationChancePercent);

    // TTS menu (Twitch/TTS/Stream/About menu bar): the checkable actions are
    // just another way to toggle the same checkboxes, kept in sync both ways.
    connect(ui->ttsCheckBox, &QCheckBox::toggled, ui->actionReading, &QAction::setChecked);
    connect(ui->actionReading, &QAction::toggled, ui->ttsCheckBox, &QCheckBox::setChecked);
    connect(ui->readEveryNCheckBox, &QCheckBox::toggled, ui->actionSkip_messages, &QAction::setChecked);
    connect(ui->actionSkip_messages, &QAction::toggled, ui->readEveryNCheckBox, &QCheckBox::setChecked);
    connect(ui->saturationCheckBox, &QCheckBox::toggled, ui->actionSaturation, &QAction::setChecked);
    connect(ui->actionSaturation, &QAction::toggled, ui->saturationCheckBox, &QCheckBox::setChecked);

    // Loads saved preferences (if any): since the connections above are
    // already in place, the loaded values immediately propagate to m_tts.
    // The auto-save connections are only wired up afterwards, so as not to
    // rewrite the file with the same values just read from it.
    loadSettings();

    connect(ui->ttsCheckBox, &QCheckBox::toggled, this, &MainWindow::saveSettings);
    connect(ui->readEveryNCheckBox, &QCheckBox::toggled, this, &MainWindow::saveSettings);
    connect(ui->readEveryNSpinBox, &QSpinBox::valueChanged, this, &MainWindow::saveSettings);
    connect(ui->saturationCheckBox, &QCheckBox::toggled, this, &MainWindow::saveSettings);
    connect(ui->saturationFactorSpinBox, &QSpinBox::valueChanged, this, &MainWindow::saveSettings);
    connect(ui->saturationChanceSpinBox, &QSpinBox::valueChanged, this, &MainWindow::saveSettings);

    // Twitch login (OAuth "device code"): it drives chat reading (started
    // automatically once authenticated, whether it's a fresh login or a
    // restored session), no need to type a channel name by hand anymore.
    m_twitchAuth = new TwitchAuth(this);
    connect(ui->twitchLoginButton, &QPushButton::clicked, this, &MainWindow::onTwitchLoginButtonClicked);
    connect(m_twitchAuth, &TwitchAuth::authenticated, this, &MainWindow::onTwitchAuthenticated);
    connect(m_twitchAuth, &TwitchAuth::authFailed, this, &MainWindow::onTwitchAuthFailed);
    connect(m_twitchAuth, &TwitchAuth::loggedOut, this, &MainWindow::onTwitchLoggedOut);

    // Twitch menu: both items drive the same toggle logic as the button
    // (log in if logged out, disconnect if logged in) - only one of the two
    // is ever visible at a time (see onTwitchAuthenticated/onTwitchLoggedOut).
    // Reconnect (Chat) is wired further down, alongside chatReconnectButton.
    connect(ui->actionLog_in, &QAction::triggered, this, &MainWindow::onTwitchLoginButtonClicked);
    connect(ui->actionDisconnect, &QAction::triggered, this, &MainWindow::onTwitchLoginButtonClicked);

    // About menu.
    connect(ui->actionAboutLutherTools, &QAction::triggered, this, &MainWindow::onAboutActionTriggered);

    // Stream info (title/category/tags): read/write via the Helix API,
    // enabled only once connected to Twitch.
    m_twitchChannel = new TwitchChannel(m_twitchAuth, this);
    connect(m_twitchChannel, &TwitchChannel::infoReceived, this, &MainWindow::onStreamInfoReceived);
    connect(m_twitchChannel, &TwitchChannel::infoFailed, this, &MainWindow::onStreamInfoFailed);
    connect(m_twitchChannel, &TwitchChannel::updateSucceeded, this, &MainWindow::onStreamUpdateSucceeded);
    connect(m_twitchChannel, &TwitchChannel::updateFailed, this, &MainWindow::onStreamUpdateFailed);
    connect(m_twitchChannel, &TwitchChannel::categoriesFound, this, &MainWindow::onCategoriesFound);
    connect(m_twitchChannel, &TwitchChannel::messageDeleted, this, [](const QString &messageId) {
        logger.info("Twitch message deletion confirmed: id=" + messageId.toStdString());
    });
    connect(m_twitchChannel, &TwitchChannel::messageDeleteFailed, this, [](const QString &reason) {
        logger.warning("Twitch message deletion failed: " + reason.toStdString());
    });

    connect(ui->streamFetchButton, &QPushButton::clicked, this, &MainWindow::onStreamFetchButtonClicked);
    // Stream menu: same action as the "Load from Twitch" button.
    connect(ui->actionLoad_from_Twitch, &QAction::triggered, this, &MainWindow::onStreamFetchButtonClicked);
    connect(ui->streamSaveButton, &QPushButton::clicked, this, &MainWindow::onStreamSaveButtonClicked);
    connect(ui->streamCategoryEdit, &QLineEdit::textEdited, this, &MainWindow::onStreamCategoryTextEdited);
    connect(ui->streamTagsInputEdit, &QLineEdit::returnPressed, this, &MainWindow::onStreamTagInputReturnPressed);

    m_categoryModel = new QStringListModel(this);
    m_categoryCompleter = new QCompleter(m_categoryModel, this);
    m_categoryCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    ui->streamCategoryEdit->setCompleter(m_categoryCompleter);

    // Twitch-style tags: "chips" that flow and wrap to the next line
    // (FlowLayout), the text field only being used to add a new one.
    m_tagsFlowLayout = new FlowLayout(ui->streamTagsChipsWidget, 0, 6, 6);

    // Stream configuration presets: purely local, independent of Twitch
    // (loaded right at launch, not only once connected). Each preset in the
    // list has its own buttons (info/load/delete), built dynamically in
    // refreshPresetList().
    connect(ui->presetSaveButton, &QPushButton::clicked, this, &MainWindow::onPresetSaveButtonClicked);
    loadPresets();

    // Sending messages in chat, as the connected channel.
    connect(ui->chatSendButton, &QPushButton::clicked, this, &MainWindow::onChatSendButtonClicked);
    connect(ui->chatSendEdit, &QLineEdit::returnPressed, this, &MainWindow::onChatSendButtonClicked);

    // Reconnecting the chat: same action, whether triggered from the button
    // above the chat panel or from the Twitch menu.
    connect(ui->chatReconnectButton, &QPushButton::clicked, this, &MainWindow::onChatReconnectButtonClicked);
    connect(ui->actionReconnect_Chat, &QAction::triggered, this, &MainWindow::onChatReconnectButtonClicked);

    // Muted users: purely local (like presets), independent of Twitch -
    // same dialog whether opened from the Settings panel or the TTS menu.
    connect(ui->mutedUsersButton, &QPushButton::clicked, this, &MainWindow::onMutedUsersButtonClicked);
    connect(ui->actionMutedUsers, &QAction::triggered, this, &MainWindow::onMutedUsersButtonClicked);
    loadMutedUsers();

    // Banned words: same pattern as muted users - just the save/edit system
    // for now, not yet applied to incoming messages.
    connect(ui->bannedWordsButton, &QPushButton::clicked, this, &MainWindow::onBannedWordsButtonClicked);
    connect(ui->actionBannedWords, &QAction::triggered, this, &MainWindow::onBannedWordsButtonClicked);
    loadBannedWords();

    m_twitchAuth->restoreSession();
}

MainWindow::~MainWindow()
{
    // m_network may have a request in flight (typically the Twitch avatar
    // download) at the time of closing. If QObject's implicit cleanup is
    // left to handle it, that only happens after this destructor's body
    // (i.e. after "delete ui" below): deleting it then could synchronously
    // trigger the in-flight reply's finished() signal (direct connection,
    // same thread), and the connected lambda would touch
    // ui->twitchAvatarLabel while it's already destroyed -> "destructor may
    // have already run". So it's deleted first, while ui is still valid.
    //
    // Same issue, indirectly, with child objects (TwitchAuth especially,
    // with its own possible in-flight network request): their implicit
    // destruction by QObject also only happens after this destructor, and
    // could still emit a signal (authFailed, etc.) that would reach one of
    // our slots touching ui. So every connection from children back to us
    // is cut first.
    if (m_twitchAuth)
        m_twitchAuth->disconnect(this);
    if (m_twitchChannel)
        m_twitchChannel->disconnect(this);
    if (m_tts)
        m_tts->disconnect(this);
    if (m_chat)
        m_chat->disconnect(this);

    delete m_network;
    m_network = nullptr;

    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::loadSettings()
{
    const QSettings settings(appSettingsFilePath(), QSettings::IniFormat);

    ui->ttsCheckBox->setChecked(
        settings.value(QStringLiteral("ttsEnabled"), ui->ttsCheckBox->isChecked()).toBool());
    ui->readEveryNCheckBox->setChecked(
        settings.value(QStringLiteral("skipEnabled"), ui->readEveryNCheckBox->isChecked()).toBool());
    ui->readEveryNSpinBox->setValue(
        settings.value(QStringLiteral("skipRate"), ui->readEveryNSpinBox->value()).toInt());
    ui->saturationCheckBox->setChecked(
        settings.value(QStringLiteral("saturationEnabled"), ui->saturationCheckBox->isChecked()).toBool());
    ui->saturationFactorSpinBox->setValue(
        settings.value(QStringLiteral("saturationFactor"), ui->saturationFactorSpinBox->value()).toInt());
    ui->saturationChanceSpinBox->setValue(
        settings.value(QStringLiteral("saturationChancePercent"), ui->saturationChanceSpinBox->value())
            .toInt());
}

void MainWindow::saveSettings()
{
    QSettings settings(appSettingsFilePath(), QSettings::IniFormat);

    settings.setValue(QStringLiteral("ttsEnabled"), ui->ttsCheckBox->isChecked());
    settings.setValue(QStringLiteral("skipEnabled"), ui->readEveryNCheckBox->isChecked());
    settings.setValue(QStringLiteral("skipRate"), ui->readEveryNSpinBox->value());
    settings.setValue(QStringLiteral("saturationEnabled"), ui->saturationCheckBox->isChecked());
    settings.setValue(QStringLiteral("saturationFactor"), ui->saturationFactorSpinBox->value());
    settings.setValue(QStringLiteral("saturationChancePercent"), ui->saturationChanceSpinBox->value());
}

void MainWindow::attachChat(Chat *chat)
{
    delete m_chat;
    m_chat = chat;

    connect(m_chat, &Chat::messageReceived, this, &MainWindow::onChatMessageReceived);
    connect(m_chat, &Chat::statusChanged, this, &MainWindow::onChatStatusChanged);
}

void MainWindow::onChatMessageReceived(const QString &username, const QString &message, bool isModerator,
                                        const QString &messageId)
{
    // Banned words: a non-moderator's message (isModerator is also true for
    // our own messages, see onChatSendButtonClicked) containing one is
    // auto-deleted on Twitch and never read - but it still shows up in the
    // software's own chat panel, just marked as deleted, so there's a trace
    // of what happened.
    const QString bannedWord = isModerator ? QString() : firstBannedWord(message);
    if (!bannedWord.isEmpty()) {
        logger.warning("Auto-deleted message from " + username.toStdString() + " (banned word \""
                        + bannedWord.toStdString() + "\"): " + message.toStdString());

        if (m_twitchChannel && !messageId.isEmpty()) {
            m_twitchChannel->deleteMessage(messageId);
        } else {
            logger.warning("Cannot delete message on Twitch: no message id available "
                            "(IRCv3 tags not received for this line?)");
        }

        ui->chatDisplay->append(QStringLiteral("<b>(deleted) %1</b> : %2")
                                     .arg(username.toHtmlEscaped(), message.toHtmlEscaped()));
        return;
    }

    ui->chatDisplay->append(
        QStringLiteral("<b>%1</b> : %2").arg(username.toHtmlEscaped(), message.toHtmlEscaped()));

    // Muted users still show up in the chat panel above - they're just
    // never forwarded to the TTS queue.
    if (ui->ttsCheckBox->isChecked() && !isUserMuted(username)) {
        logger.info("Reading message from " + username.toStdString() + ": " + message.toStdString());
        m_tts->enqueue(message);
    }
}

void MainWindow::onChatStatusChanged(const QString &status)
{
    // Already visible in the Chat panel itself (line below): the status bar
    // stays dedicated to the permanent branding message (see constructor)
    // rather than these transient statuses.
    ui->chatDisplay->append(QStringLiteral("<i>%1</i>").arg(status.toHtmlEscaped()));
}

void MainWindow::onChatSendButtonClicked()
{
    const QString message = ui->chatSendEdit->text().trimmed();
    if (message.isEmpty() || !m_chat)
        return;

    m_chat->sendMessage(message);

    // Twitch's IRC server doesn't echo our own messages back: their receipt
    // is simulated via the same path as any other message (display + TTS
    // reading if enabled), for behavior identical to a message sent from
    // Twitch directly. isModerator=true: it's our own message, always exempt
    // from the banned-words filter; no messageId, so it could never trigger
    // a deletion anyway.
    onChatMessageReceived(m_twitchAuth->login(), message, true, QString());

    ui->chatSendEdit->clear();
}

void MainWindow::onChatReconnectButtonClicked()
{
    if (m_chat)
        m_chat->reconnect();
}

void MainWindow::onTwitchLoginButtonClicked()
{
    if (m_twitchAuth->isAuthenticated()) {
        if (m_chat)
            m_chat->disconnectFromChat();
        m_twitchAuth->logout();
        return;
    }

    ui->twitchLoginButton->setEnabled(false);
    m_twitchAuth->startLogin();
}

void MainWindow::onTwitchAuthenticated(const QString &login, const QString &displayName,
                                        const QString &avatarUrl)
{
    ui->twitchLoginButton->setEnabled(true);
    ui->twitchLoginButton->setText(tr("Disconnect Twitch (%1)").arg(login));

    ui->actionLog_in->setVisible(false);
    ui->actionDisconnect->setVisible(true);

    ui->twitchDisplayNameLabel->setText(displayName);
    ui->twitchDisplayNameLabel->setVisible(true);

    if (!avatarUrl.isEmpty()) {
        QNetworkReply *reply = m_network->get(QNetworkRequest(QUrl(avatarUrl)));
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError)
                return;

            QPixmap avatar;
            if (avatar.loadFromData(reply->readAll())) {
                ui->twitchAvatarLabel->setPixmap(avatar);
                ui->twitchAvatarLabel->setVisible(true);
            }
        });
    }

    // Chat reading starts automatically with the authenticated channel.
    // Sending messages requires the chat:edit scope: an older session
    // (logged in before it was added) might not have it yet -> falls back
    // to an anonymous connection (read-only, as before) rather than risking
    // an IRC connection rejected by Twitch.
    const bool canSendChat = m_twitchAuth->hasScope(QStringLiteral("chat:edit"));
    if (canSendChat)
        attachChat(new Chat(login, login, m_twitchAuth->accessToken(), this));
    else
        attachChat(new Chat(login, QString(), QString(), this));
    m_chat->connectToChat();

    // Reconnecting doesn't need the chat:edit scope (it works the same for
    // an anonymous, read-only connection), so it's enabled regardless of
    // canSendChat.
    ui->chatReconnectButton->setEnabled(true);
    ui->actionReconnect_Chat->setEnabled(true);

    ui->chatSendEdit->setEnabled(canSendChat);
    ui->chatSendButton->setEnabled(canSendChat);
    if (!canSendChat) {
        ui->chatDisplay->append(tr("<i>Reconnect to Twitch to be able to write in chat "
                                    "(additional authorization required).</i>"));
    }

    setStreamInfoEnabled(true);
    m_twitchChannel->fetchInfo();
}

void MainWindow::onTwitchAuthFailed(const QString &reason)
{
    ui->twitchLoginButton->setEnabled(true);
    ui->statusbar->showMessage(reason, 8000);
}

void MainWindow::onTwitchLoggedOut()
{
    ui->twitchLoginButton->setEnabled(true);
    ui->twitchLoginButton->setText(tr("Log in with Twitch"));

    ui->actionLog_in->setVisible(true);
    ui->actionDisconnect->setVisible(false);

    ui->twitchDisplayNameLabel->setVisible(false);
    ui->twitchAvatarLabel->setVisible(false);

    ui->chatReconnectButton->setEnabled(false);
    ui->actionReconnect_Chat->setEnabled(false);

    ui->chatSendEdit->setEnabled(false);
    ui->chatSendEdit->clear();
    ui->chatSendButton->setEnabled(false);

    setStreamInfoEnabled(false);
}

void MainWindow::setStreamInfoEnabled(bool enabled)
{
    ui->streamTitleEdit->setEnabled(enabled);
    ui->streamCategoryEdit->setEnabled(enabled);
    ui->streamTagsInputEdit->setEnabled(enabled);
    ui->streamFetchButton->setEnabled(enabled);
    ui->streamSaveButton->setEnabled(enabled);
    ui->actionLoad_from_Twitch->setEnabled(enabled);

    // Presets only serve to fill in the fields above, themselves unusable
    // while logged out: so they follow the same state. The locally saved
    // list (m_presets), however, is not cleared - it stays available for
    // the next login.
    ui->presetListScrollArea->setEnabled(enabled);
    ui->presetNameEdit->setEnabled(enabled);
    ui->presetSaveButton->setEnabled(enabled);

    if (!enabled) {
        ui->streamTitleEdit->clear();
        ui->streamCategoryEdit->clear();
        ui->streamTagsInputEdit->clear();
        ui->streamStatusLabel->clear();
        m_categoryIdByName.clear();
        m_categoryModel->setStringList({});
        m_currentGameId.clear();
        m_currentTags.clear();
        rebuildTagChips();

        ui->presetNameEdit->clear();
    }
}

QString MainWindow::resolveCategoryId(const QString &categoryText) const
{
    for (auto it = m_categoryIdByName.constBegin(); it != m_categoryIdByName.constEnd(); ++it) {
        if (it.key().compare(categoryText, Qt::CaseInsensitive) == 0)
            return it.value();
    }
    return QString();
}

void MainWindow::onStreamInfoReceived(const QString &title, const QString &gameName, const QString &gameId,
                                       const QStringList &tags)
{
    ui->streamTitleEdit->setText(title);
    ui->streamCategoryEdit->setText(gameName);
    m_currentTags = tags;
    rebuildTagChips();

    m_currentGameId = gameId;
    if (!gameName.isEmpty())
        m_categoryIdByName.insert(gameName, gameId);

    ui->streamStatusLabel->clear();
}

void MainWindow::onStreamInfoFailed(const QString &reason)
{
    ui->streamStatusLabel->setText(tr("Failed to load: %1").arg(reason));
}

void MainWindow::onStreamUpdateSucceeded()
{
    ui->streamSaveButton->setEnabled(true);
    ui->streamStatusLabel->setText(tr("Saved to Twitch."));
}

void MainWindow::onStreamUpdateFailed(const QString &reason)
{
    ui->streamSaveButton->setEnabled(true);
    ui->streamStatusLabel->setText(tr("Failed to save: %1").arg(reason));
}

void MainWindow::onStreamFetchButtonClicked()
{
    ui->streamStatusLabel->setText(tr("Loading..."));
    m_twitchChannel->fetchInfo();
}

void MainWindow::onStreamSaveButtonClicked()
{
    const QString title = ui->streamTitleEdit->text().trimmed();
    if (title.isEmpty()) {
        ui->streamStatusLabel->setText(tr("The title can't be empty."));
        return;
    }

    // Tags are already validated (length, count) when added as chips:
    // m_currentTags can be used directly.
    const QStringList &tags = m_currentTags;

    // Category: left unchanged on Twitch's side if the field is empty;
    // otherwise it needs to be resolved into a game_id among the names
    // already seen (initial load + search results), since Twitch doesn't
    // accept a raw name.
    const QString categoryText = ui->streamCategoryEdit->text().trimmed();
    QString gameId;
    if (!categoryText.isEmpty()) {
        gameId = resolveCategoryId(categoryText);
        if (gameId.isEmpty()) {
            ui->streamStatusLabel->setText(
                tr("Category \"%1\" not recognized: pick it from the suggested list.").arg(categoryText));
            return;
        }
    }

    ui->streamSaveButton->setEnabled(false);
    ui->streamStatusLabel->setText(tr("Saving..."));
    m_twitchChannel->updateInfo(title, gameId, tags);
}

void MainWindow::onStreamCategoryTextEdited(const QString &text)
{
    Q_UNUSED(text);

    // Debounce: the Twitch search only fires once typing has paused, not on
    // every keystroke.
    if (!m_categorySearchTimer) {
        m_categorySearchTimer = new QTimer(this);
        m_categorySearchTimer->setSingleShot(true);
        connect(m_categorySearchTimer, &QTimer::timeout, this,
                [this]() { m_twitchChannel->searchCategories(ui->streamCategoryEdit->text()); });
    }
    m_categorySearchTimer->start(300);
}

void MainWindow::onCategoriesFound(const QVector<QPair<QString, QString>> &categories)
{
    QStringList names;
    names.reserve(categories.size());
    for (const auto &category : categories) {
        m_categoryIdByName.insert(category.first, category.second);
        names << category.first;
    }
    m_categoryModel->setStringList(names);
}

void MainWindow::onStreamTagInputReturnPressed()
{
    addTag(ui->streamTagsInputEdit->text());
    ui->streamTagsInputEdit->clear();
}

void MainWindow::addTag(const QString &rawTag)
{
    const QString tag = rawTag.trimmed();
    if (tag.isEmpty())
        return;

    if (tag.size() > 25) {
        ui->streamStatusLabel->setText(tr("The tag \"%1\" is over 25 characters.").arg(tag));
        return;
    }
    if (m_currentTags.contains(tag, Qt::CaseInsensitive))
        return;
    if (m_currentTags.size() >= 10) {
        ui->streamStatusLabel->setText(tr("10 tags maximum."));
        return;
    }

    m_currentTags << tag;
    ui->streamStatusLabel->clear();
    rebuildTagChips();
}

void MainWindow::removeTag(const QString &tag)
{
    m_currentTags.removeAll(tag);
    rebuildTagChips();
}

void MainWindow::rebuildTagChips()
{
    QLayoutItem *item;
    while ((item = m_tagsFlowLayout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    for (const QString &tag : std::as_const(m_currentTags)) {
        auto *chip = new QWidget(ui->streamTagsChipsWidget);
        chip->setStyleSheet(QStringLiteral("background-color: #f0f0f0; border: 1px solid #a0a0a0;"));

        auto *chipLayout = new QHBoxLayout(chip);
        chipLayout->setContentsMargins(8, 3, 4, 3);
        chipLayout->setSpacing(4);

        auto *label = new QLabel(tag, chip);
        label->setStyleSheet(QStringLiteral("color: black; background: transparent; border: none;"));
        chipLayout->addWidget(label);

        auto *closeButton = new QToolButton(chip);
        closeButton->setText(QStringLiteral("×"));
        closeButton->setCursor(Qt::PointingHandCursor);
        closeButton->setStyleSheet(QStringLiteral(
            "QToolButton { color: #333333; background: transparent; border: none; font-weight: bold; }"
            "QToolButton:hover { color: black; background-color: #d0d0d0; }"));
        connect(closeButton, &QToolButton::clicked, this, [this, tag]() { removeTag(tag); });
        chipLayout->addWidget(closeButton);

        m_tagsFlowLayout->addWidget(chip);
    }
}

void MainWindow::onPresetSaveButtonClicked()
{
    const QString name = ui->presetNameEdit->text().trimmed();
    if (name.isEmpty()) {
        ui->streamStatusLabel->setText(tr("The preset name can't be empty."));
        return;
    }

    StreamPreset preset;
    preset.name = name;
    preset.title = ui->streamTitleEdit->text();
    preset.gameName = ui->streamCategoryEdit->text().trimmed();
    preset.gameId = preset.gameName.isEmpty() ? QString() : resolveCategoryId(preset.gameName);
    preset.tags = m_currentTags;

    // A preset with the same name (case-insensitive) is updated rather than
    // duplicated.
    const int existingIndex = presetIndexByName(name);
    if (existingIndex >= 0) {
        m_presets[existingIndex] = preset;
    } else {
        if (m_presets.size() >= kMaxPresets) {
            ui->streamStatusLabel->setText(tr("%1 presets maximum.").arg(kMaxPresets));
            return;
        }
        m_presets.append(preset);
    }

    savePresets();
    refreshPresetList();
    ui->presetNameEdit->clear();
    ui->streamStatusLabel->setText(tr("Preset \"%1\" saved.").arg(name));
}

int MainWindow::presetIndexByName(const QString &name) const
{
    for (int i = 0; i < m_presets.size(); ++i) {
        if (m_presets.at(i).name.compare(name, Qt::CaseInsensitive) == 0)
            return i;
    }
    return -1;
}

void MainWindow::loadPreset(const QString &name)
{
    const int index = presetIndexByName(name);
    if (index < 0)
        return;

    const StreamPreset &preset = m_presets.at(index);
    ui->streamTitleEdit->setText(preset.title);
    ui->streamCategoryEdit->setText(preset.gameName);

    m_currentTags = preset.tags;
    rebuildTagChips();

    // If the preset already knows its category's game_id, it's reinjected
    // so an immediate "Save to Twitch" doesn't need a fresh search to
    // resolve it.
    if (!preset.gameName.isEmpty() && !preset.gameId.isEmpty()) {
        m_categoryIdByName.insert(preset.gameName, preset.gameId);
        m_currentGameId = preset.gameId;
    }

    ui->streamStatusLabel->setText(tr("Preset \"%1\" loaded.").arg(preset.name));
}

void MainWindow::deletePreset(const QString &name)
{
    const int index = presetIndexByName(name);
    if (index < 0)
        return;

    // Irreversible deletion: asks for confirmation, with explicit button
    // text (a standard QMessageBox's buttons follow the system locale,
    // which might not match the app's).
    QMessageBox confirm(this);
    confirm.setWindowTitle(tr("Delete preset"));
    confirm.setText(tr("Delete the preset \"%1\"? This action is irreversible.").arg(name));
    confirm.setIcon(QMessageBox::Warning);
    QPushButton *deleteButton = confirm.addButton(tr("Delete"), QMessageBox::DestructiveRole);
    confirm.addButton(tr("Cancel"), QMessageBox::RejectRole);
    confirm.exec();
    if (confirm.clickedButton() != deleteButton)
        return;

    m_presets.removeAt(index);
    savePresets();
    refreshPresetList();
}

void MainWindow::showPresetInfoDialog(const QString &name)
{
    const int index = presetIndexByName(name);
    if (index < 0)
        return;

    // Working copy: changes only touch m_presets once validated (the
    // "Save" button on the micro pop-up).
    StreamPreset preset = m_presets.at(index);

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Preset: %1").arg(preset.name));

    auto *layout = new QVBoxLayout(&dialog);

    layout->addWidget(new QLabel(tr("Name"), &dialog));
    auto *nameEdit = new QLineEdit(preset.name, &dialog);
    layout->addWidget(nameEdit);

    layout->addWidget(new QLabel(tr("Title"), &dialog));
    auto *titleEdit = new QLineEdit(preset.title, &dialog);
    titleEdit->setMaxLength(140);
    layout->addWidget(titleEdit);

    layout->addWidget(new QLabel(tr("Category"), &dialog));
    auto *categoryEdit = new QLineEdit(preset.gameName, &dialog);
    layout->addWidget(categoryEdit);

    layout->addWidget(new QLabel(tr("Tags (comma-separated)"), &dialog));
    auto *tagsEdit = new QLineEdit(preset.tags.join(QStringLiteral(", ")), &dialog);
    layout->addWidget(tagsEdit);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    // QDialogButtonBox translates its standard buttons according to the
    // system locale: explicit text keeps it consistent with the rest of
    // the interface.
    buttonBox->button(QDialogButtonBox::Save)->setText(tr("Save"));
    buttonBox->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    layout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, [&]() {
        const QString newName = nameEdit->text().trimmed();
        if (newName.isEmpty()) {
            QMessageBox::warning(&dialog, tr("Preset"), tr("The preset name can't be empty."));
            return;
        }
        const int conflictIndex = presetIndexByName(newName);
        if (conflictIndex >= 0 && conflictIndex != index) {
            QMessageBox::warning(&dialog, tr("Preset"),
                                  tr("A preset named \"%1\" already exists.").arg(newName));
            return;
        }

        QStringList tags;
        const QStringList rawTags = tagsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &rawTag : rawTags) {
            const QString tag = rawTag.trimmed();
            if (tag.isEmpty())
                continue;
            if (tag.size() > 25) {
                QMessageBox::warning(&dialog, tr("Preset"),
                                      tr("The tag \"%1\" is over 25 characters.").arg(tag));
                return;
            }
            tags << tag;
        }
        if (tags.size() > 10) {
            QMessageBox::warning(&dialog, tr("Preset"), tr("10 tags maximum (%1 given).").arg(tags.size()));
            return;
        }

        const QString newGameName = categoryEdit->text().trimmed();
        preset.name = newName;
        preset.title = titleEdit->text();
        preset.gameName = newGameName;
        // Category unchanged: keep the already-known game_id instead of
        // losing it (a fresh resolution would require a search).
        preset.gameId = newGameName.compare(m_presets.at(index).gameName, Qt::CaseInsensitive) == 0
                             ? m_presets.at(index).gameId
                             : resolveCategoryId(newGameName);
        preset.tags = tags;

        m_presets[index] = preset;
        savePresets();
        refreshPresetList();

        dialog.accept();
    });

    dialog.exec();
}

void MainWindow::loadPresets()
{
    const QSettings settings(appSettingsFilePath(), QSettings::IniFormat);
    const QByteArray json = settings.value(QStringLiteral("streamPresets")).toString().toUtf8();
    const QJsonArray array = QJsonDocument::fromJson(json).array();

    m_presets.clear();
    for (const QJsonValue &v : array) {
        const QJsonObject obj = v.toObject();

        StreamPreset preset;
        preset.name = obj.value(QStringLiteral("name")).toString();
        preset.title = obj.value(QStringLiteral("title")).toString();
        preset.gameName = obj.value(QStringLiteral("gameName")).toString();
        preset.gameId = obj.value(QStringLiteral("gameId")).toString();
        for (const QJsonValue &t : obj.value(QStringLiteral("tags")).toArray())
            preset.tags << t.toString();

        if (!preset.name.isEmpty())
            m_presets.append(preset);
    }

    refreshPresetList();
}

void MainWindow::savePresets()
{
    QJsonArray array;
    for (const StreamPreset &preset : std::as_const(m_presets)) {
        QJsonObject obj;
        obj.insert(QStringLiteral("name"), preset.name);
        obj.insert(QStringLiteral("title"), preset.title);
        obj.insert(QStringLiteral("gameName"), preset.gameName);
        obj.insert(QStringLiteral("gameId"), preset.gameId);

        QJsonArray tagsArray;
        for (const QString &tag : preset.tags)
            tagsArray.append(tag);
        obj.insert(QStringLiteral("tags"), tagsArray);

        array.append(obj);
    }

    QSettings settings(appSettingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("streamPresets"),
                       QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

void MainWindow::refreshPresetList()
{
    // A plain QVBoxLayout in a QScrollArea rather than a QListWidget +
    // setItemWidget(): the latter would only recompute each row's width
    // (and thus the delete cross's position, all the way to the right) on
    // the widget's first real resize - never on the initial fill, even
    // deferred. A "normal" layout like this one is handled by Qt's standard
    // layout mechanism and doesn't have that flaw.
    QLayoutItem *oldItem;
    while ((oldItem = ui->presetListContentLayout->takeAt(0)) != nullptr) {
        delete oldItem->widget();
        delete oldItem;
    }

    for (const StreamPreset &preset : std::as_const(m_presets)) {
        const QString name = preset.name;

        auto *row = new QWidget(ui->presetListScrollAreaContents);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(2, 1, 2, 1);
        rowLayout->setSpacing(2);

        const QString buttonStyle = QStringLiteral("QToolButton { padding: 0px 3px; }");

        auto *nameLabel = new QLabel(row);
        nameLabel->setText(row->fontMetrics().elidedText(name, Qt::ElideRight, 145));
        nameLabel->setToolTip(name);
        rowLayout->addWidget(nameLabel, 1);

        auto *infoButton = new QToolButton(row);
        infoButton->setText(QStringLiteral("i"));
        infoButton->setToolTip(tr("View / edit the preset's info"));
        infoButton->setStyleSheet(buttonStyle);
        connect(infoButton, &QToolButton::clicked, this, [this, name]() { showPresetInfoDialog(name); });
        rowLayout->addWidget(infoButton);

        auto *loadButton = new QToolButton(row);
        loadButton->setText(tr("Load"));
        loadButton->setStyleSheet(buttonStyle);
        connect(loadButton, &QToolButton::clicked, this, [this, name]() { loadPreset(name); });
        rowLayout->addWidget(loadButton);

        // Delete cross, same style as on the tags.
        auto *deleteButton = new QToolButton(row);
        deleteButton->setText(QStringLiteral("×"));
        deleteButton->setToolTip(tr("Delete the preset"));
        deleteButton->setCursor(Qt::PointingHandCursor);
        deleteButton->setStyleSheet(QStringLiteral(
            "QToolButton { color: #333333; background: transparent; border: none; font-weight: bold; }"
            "QToolButton:hover { color: black; background-color: #d0d0d0; }"));
        connect(deleteButton, &QToolButton::clicked, this, [this, name]() { deletePreset(name); });
        rowLayout->addWidget(deleteButton);

        ui->presetListContentLayout->addWidget(row);
    }

    ui->presetListContentLayout->addStretch();
}

void MainWindow::onAboutActionTriggered()
{
    // To credit someone else or for a new area of the app, add a line here:
    // either a new "<br>role" under an existing person's <b>Name</b> block,
    // or a whole new "<p><b>Name</b><br>role</p>" block for a new person.
    QMessageBox::about(
        this, tr("About LutherTools"),
        tr("<h3>LutherTools v%1</h3>"
           "<p><b>Corentin BOUTIGNY</b><br>"
           "Twitch connection<br>"
           "TTS<br>"
           "Presets<br>"
           "Moderation</p>"
           "<p><b>Benjamin DESCOURS--TERRIER</b><br>"
           "Linux port<br>"
           "Logger</p>")
            .arg(QStringLiteral(LUTHERTOOLS_VERSION_STRING)));
}

void MainWindow::onMutedUsersButtonClicked()
{
    showMutedUsersDialog();
}

void MainWindow::loadMutedUsers()
{
    const QSettings settings(appSettingsFilePath(), QSettings::IniFormat);
    m_mutedUsers = settings.value(QStringLiteral("mutedUsers")).toStringList();
}

void MainWindow::saveMutedUsers()
{
    QSettings settings(appSettingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("mutedUsers"), m_mutedUsers);
}

void MainWindow::removeMutedUser(const QString &username)
{
    logger.info("Muted user removed: " + username.toStdString());
    m_mutedUsers.removeAll(username);
    saveMutedUsers();
}

bool MainWindow::isUserMuted(const QString &username) const
{
    return m_mutedUsers.contains(username, Qt::CaseInsensitive);
}

void MainWindow::showMutedUsersDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Muted users"));
    dialog.resize(260, 340);

    auto *layout = new QVBoxLayout(&dialog);

    auto *description =
        new QLabel(tr("Messages from these users are still shown in chat, but never read aloud by TTS."), &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);

    // Same scrollable list-of-rows pattern as the presets list.
    auto *scrollArea = new QScrollArea(&dialog);
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *scrollContents = new QWidget(scrollArea);
    auto *listLayout = new QVBoxLayout(scrollContents);
    listLayout->setSpacing(0);
    listLayout->setContentsMargins(0, 0, 0, 0);
    scrollArea->setWidget(scrollContents);
    layout->addWidget(scrollArea, 1);

    // std::function (rather than a plain lambda) so it can call itself -
    // needed to redraw the list after every add/remove.
    std::function<void()> rebuildList;
    rebuildList = [this, listLayout, scrollContents, &rebuildList]() {
        QLayoutItem *item;
        while ((item = listLayout->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }

        for (const QString &user : std::as_const(m_mutedUsers)) {
            auto *row = new QWidget(scrollContents);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(2, 1, 2, 1);
            rowLayout->setSpacing(2);

            auto *nameLabel = new QLabel(user, row);
            rowLayout->addWidget(nameLabel, 1);

            auto *removeButton = new QToolButton(row);
            removeButton->setText(QStringLiteral("×"));
            removeButton->setToolTip(tr("Unmute this user"));
            removeButton->setCursor(Qt::PointingHandCursor);
            removeButton->setStyleSheet(QStringLiteral(
                "QToolButton { color: #333333; background: transparent; border: none; font-weight: bold; }"
                "QToolButton:hover { color: black; background-color: #d0d0d0; }"));
            connect(removeButton, &QToolButton::clicked, this, [this, user, &rebuildList]() {
                removeMutedUser(user);
                rebuildList();
            });
            rowLayout->addWidget(removeButton);

            listLayout->addWidget(row);
        }

        listLayout->addStretch();
    };
    rebuildList();

    auto *addLayout = new QHBoxLayout();
    auto *addEdit = new QLineEdit(&dialog);
    addEdit->setPlaceholderText(tr("Twitch username"));
    addLayout->addWidget(addEdit);
    auto *addButton = new QPushButton(tr("Add"), &dialog);
    addLayout->addWidget(addButton);
    layout->addLayout(addLayout);

    // Stored/displayed lowercase: chat usernames are always lowercase IRC
    // logins (e.g. "wizebot" for the channel bot shown as "WZBot"), so
    // matching against them only works if entered the same way.
    auto addUser = [this, addEdit, &rebuildList]() {
        const QString name = addEdit->text().trimmed().toLower();
        if (name.isEmpty())
            return;
        if (m_mutedUsers.contains(name, Qt::CaseInsensitive)) {
            addEdit->clear();
            return;
        }

        logger.info("Muted user added: " + name.toStdString());
        m_mutedUsers << name;
        saveMutedUsers();
        addEdit->clear();
        rebuildList();
    };
    connect(addButton, &QPushButton::clicked, &dialog, addUser);
    connect(addEdit, &QLineEdit::returnPressed, &dialog, addUser);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttonBox->button(QDialogButtonBox::Close)->setText(tr("Close"));
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttonBox);

    dialog.exec();
}

void MainWindow::onBannedWordsButtonClicked()
{
    showBannedWordsDialog();
}

void MainWindow::loadBannedWords()
{
    const QSettings settings(appSettingsFilePath(), QSettings::IniFormat);
    m_bannedWords = settings.value(QStringLiteral("bannedWords")).toStringList();
}

void MainWindow::saveBannedWords()
{
    QSettings settings(appSettingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("bannedWords"), m_bannedWords);
}

void MainWindow::removeBannedWord(const QString &word)
{
    logger.info("Banned word removed: " + word.toStdString());
    m_bannedWords.removeAll(word);
    saveBannedWords();
}

QString MainWindow::firstBannedWord(const QString &message) const
{
    const QString lowerMessage = message.toLower();
    for (const QString &word : m_bannedWords) {
        if (lowerMessage.contains(word))
            return word;
    }
    return QString();
}

void MainWindow::showBannedWordsDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Banned words"));
    dialog.resize(260, 340);

    auto *layout = new QVBoxLayout(&dialog);

    auto *description = new QLabel(
        tr("A non-moderator's message containing any of these words is deleted from Twitch chat and never "
           "read aloud. It still appears here, marked \"(deleted)\". Moderators and you are always exempt."),
        &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);

    // Same scrollable list-of-rows pattern as muted users/presets.
    auto *scrollArea = new QScrollArea(&dialog);
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *scrollContents = new QWidget(scrollArea);
    auto *listLayout = new QVBoxLayout(scrollContents);
    listLayout->setSpacing(0);
    listLayout->setContentsMargins(0, 0, 0, 0);
    scrollArea->setWidget(scrollContents);
    layout->addWidget(scrollArea, 1);

    std::function<void()> rebuildList;
    rebuildList = [this, listLayout, scrollContents, &rebuildList]() {
        QLayoutItem *item;
        while ((item = listLayout->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }

        for (const QString &word : std::as_const(m_bannedWords)) {
            auto *row = new QWidget(scrollContents);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(2, 1, 2, 1);
            rowLayout->setSpacing(2);

            auto *wordLabel = new QLabel(word, row);
            rowLayout->addWidget(wordLabel, 1);

            auto *removeButton = new QToolButton(row);
            removeButton->setText(QStringLiteral("×"));
            removeButton->setToolTip(tr("Remove this word"));
            removeButton->setCursor(Qt::PointingHandCursor);
            removeButton->setStyleSheet(QStringLiteral(
                "QToolButton { color: #333333; background: transparent; border: none; font-weight: bold; }"
                "QToolButton:hover { color: black; background-color: #d0d0d0; }"));
            connect(removeButton, &QToolButton::clicked, this, [this, word, &rebuildList]() {
                removeBannedWord(word);
                rebuildList();
            });
            rowLayout->addWidget(removeButton);

            listLayout->addWidget(row);
        }

        listLayout->addStretch();
    };
    rebuildList();

    auto *addLayout = new QHBoxLayout();
    auto *addEdit = new QLineEdit(&dialog);
    addEdit->setPlaceholderText(tr("Word to ban"));
    addLayout->addWidget(addEdit);
    auto *addButton = new QPushButton(tr("Add"), &dialog);
    addLayout->addWidget(addButton);
    layout->addLayout(addLayout);

    // Stored/matched lowercase so the filter is case-insensitive regardless
    // of how the word is typed here or cased in the actual message.
    auto addWord = [this, addEdit, &rebuildList]() {
        const QString word = addEdit->text().trimmed().toLower();
        if (word.isEmpty())
            return;
        if (m_bannedWords.contains(word, Qt::CaseInsensitive)) {
            addEdit->clear();
            return;
        }

        logger.info("Banned word added: " + word.toStdString());
        m_bannedWords << word;
        saveBannedWords();
        addEdit->clear();
        rebuildList();
    };
    connect(addButton, &QPushButton::clicked, &dialog, addWord);
    connect(addEdit, &QLineEdit::returnPressed, &dialog, addWord);

    auto *buttonBox2 = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttonBox2->button(QDialogButtonBox::Close)->setText(tr("Close"));
    connect(buttonBox2, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttonBox2);

    dialog.exec();
}
