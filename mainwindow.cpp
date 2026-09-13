#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "appsettings.h"
#include "chat.h"
#include "flowlayout.h"
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
#include <QSettings>
#include <QStringListModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    setWindowTitle(QStringLiteral("LutherTools v%1").arg(QStringLiteral(LUTHERTOOLS_VERSION_STRING)));

    // Message de marque permanent en bas de fenêtre. Un widget "permanent"
    // (plutôt que showMessage()) reste affiché même quand un message
    // transitoire (ex. échec de connexion Twitch) apparaît à côté.
    auto *versionLabel = new QLabel(
        QStringLiteral("LutherTools v%1 - Flit Studio").arg(QStringLiteral(LUTHERTOOLS_VERSION_STRING)), this);
    ui->statusbar->addPermanentWidget(versionLabel);

    // Connexion / Présets / Chat à parts strictement égales : même facteur
    // d'étirement pour les 3, et une répartition explicite une fois la
    // fenêtre réellement dimensionnée (avant show(), le splitter n'a pas
    // encore sa largeur finale et se contente des tailles préférées de
    // chaque volet, ce qui favorise celui qui a le plus de contrôles).
    ui->mainSplitter->setStretchFactor(0, 1);
    ui->mainSplitter->setStretchFactor(1, 1);
    ui->mainSplitter->setStretchFactor(2, 1);
    QTimer::singleShot(0, this, [this]() {
        const int third = ui->mainSplitter->width() / 3;
        ui->mainSplitter->setSizes({third, third, third});
    });

    m_network = new QNetworkAccessManager(this);
    m_tts = new TTS(this);

    // Filtre "Lire 1 message sur N" : la case active/désactive le champ.
    connect(ui->readEveryNCheckBox, &QCheckBox::toggled, ui->readEveryNSpinBox, &QSpinBox::setEnabled);
    connect(ui->readEveryNCheckBox, &QCheckBox::toggled, m_tts, &TTS::setReadEveryNEnabled);
    connect(ui->readEveryNSpinBox, &QSpinBox::valueChanged, m_tts, &TTS::setReadEveryN);

    // Filtre "Saturation aléatoire" : la case active/désactive les deux champs.
    connect(ui->saturationCheckBox, &QCheckBox::toggled, ui->saturationFactorSpinBox, &QSpinBox::setEnabled);
    connect(ui->saturationCheckBox, &QCheckBox::toggled, ui->saturationChanceSpinBox, &QSpinBox::setEnabled);
    connect(ui->saturationCheckBox, &QCheckBox::toggled, m_tts, &TTS::setSaturationEnabled);
    connect(ui->saturationFactorSpinBox, &QSpinBox::valueChanged, m_tts, &TTS::setSaturationFactor);
    connect(ui->saturationChanceSpinBox, &QSpinBox::valueChanged, m_tts, &TTS::setSaturationChancePercent);

    // Charge les préférences sauvegardées (le cas échéant) : les connexions
    // ci-dessus étant déjà en place, les valeurs chargées se répercutent tout
    // de suite sur m_tts. Les connexions de sauvegarde automatique ne sont
    // branchées qu'après, pour ne pas ré-écrire le fichier avec les mêmes
    // valeurs qu'on vient d'y lire.
    loadSettings();

    connect(ui->ttsCheckBox, &QCheckBox::toggled, this, &MainWindow::saveSettings);
    connect(ui->readEveryNCheckBox, &QCheckBox::toggled, this, &MainWindow::saveSettings);
    connect(ui->readEveryNSpinBox, &QSpinBox::valueChanged, this, &MainWindow::saveSettings);
    connect(ui->saturationCheckBox, &QCheckBox::toggled, this, &MainWindow::saveSettings);
    connect(ui->saturationFactorSpinBox, &QSpinBox::valueChanged, this, &MainWindow::saveSettings);
    connect(ui->saturationChanceSpinBox, &QSpinBox::valueChanged, this, &MainWindow::saveSettings);

    // Connexion Twitch (OAuth "device code") : c'est elle qui pilote la
    // lecture du chat (démarrée automatiquement une fois authentifié, qu'il
    // s'agisse d'une connexion fraîche ou d'une session restaurée), plus
    // besoin de saisir un nom de chaîne à la main.
    m_twitchAuth = new TwitchAuth(this);
    connect(ui->twitchLoginButton, &QPushButton::clicked, this, &MainWindow::onTwitchLoginButtonClicked);
    connect(m_twitchAuth, &TwitchAuth::authenticated, this, &MainWindow::onTwitchAuthenticated);
    connect(m_twitchAuth, &TwitchAuth::authFailed, this, &MainWindow::onTwitchAuthFailed);
    connect(m_twitchAuth, &TwitchAuth::loggedOut, this, &MainWindow::onTwitchLoggedOut);

    // Infos du stream (titre/catégorie/tags) : lecture/écriture via l'API
    // Helix, activées uniquement une fois connecté à Twitch.
    m_twitchChannel = new TwitchChannel(m_twitchAuth, this);
    connect(m_twitchChannel, &TwitchChannel::infoReceived, this, &MainWindow::onStreamInfoReceived);
    connect(m_twitchChannel, &TwitchChannel::infoFailed, this, &MainWindow::onStreamInfoFailed);
    connect(m_twitchChannel, &TwitchChannel::updateSucceeded, this, &MainWindow::onStreamUpdateSucceeded);
    connect(m_twitchChannel, &TwitchChannel::updateFailed, this, &MainWindow::onStreamUpdateFailed);
    connect(m_twitchChannel, &TwitchChannel::categoriesFound, this, &MainWindow::onCategoriesFound);

    connect(ui->streamFetchButton, &QPushButton::clicked, this, &MainWindow::onStreamFetchButtonClicked);
    connect(ui->streamSaveButton, &QPushButton::clicked, this, &MainWindow::onStreamSaveButtonClicked);
    connect(ui->streamCategoryEdit, &QLineEdit::textEdited, this, &MainWindow::onStreamCategoryTextEdited);
    connect(ui->streamTagsInputEdit, &QLineEdit::returnPressed, this, &MainWindow::onStreamTagInputReturnPressed);

    m_categoryModel = new QStringListModel(this);
    m_categoryCompleter = new QCompleter(m_categoryModel, this);
    m_categoryCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    ui->streamCategoryEdit->setCompleter(m_categoryCompleter);

    // Tags façon Twitch : des "chips" qui s'enchaînent et passent à la ligne
    // (FlowLayout), le champ texte ne servant qu'à en ajouter un nouveau.
    m_tagsFlowLayout = new FlowLayout(ui->streamTagsChipsWidget, 0, 6, 6);

    // Présets de configuration de stream : purement locaux, indépendants de
    // Twitch (chargés dès le lancement, pas seulement une fois connecté).
    // Chaque préset de la liste a ses propres boutons (info/charger/supprimer),
    // construits dynamiquement dans refreshPresetList().
    connect(ui->presetSaveButton, &QPushButton::clicked, this, &MainWindow::onPresetSaveButtonClicked);
    loadPresets();

    // Envoi de messages dans le chat, en tant que la chaîne connectée.
    connect(ui->chatSendButton, &QPushButton::clicked, this, &MainWindow::onChatSendButtonClicked);
    connect(ui->chatSendEdit, &QLineEdit::returnPressed, this, &MainWindow::onChatSendButtonClicked);

    m_twitchAuth->restoreSession();
}

MainWindow::~MainWindow()
{
    // m_network peut avoir une requête en cours (le téléchargement de
    // l'avatar Twitch, typiquement) au moment de la fermeture. Si on laisse
    // le nettoyage implicite d'QObject s'en charger, il n'arrive qu'après le
    // corps de ce destructeur (donc après "delete ui" plus bas) : sa
    // suppression peut alors déclencher le signal finished() du reply en
    // cours de façon synchrone (connexion directe, même thread), et le lambda
    // connecté toucherait ui->twitchAvatarLabel alors qu'il est déjà détruit
    // -> "destructor may have already run". On le supprime donc en premier,
    // tant que ui est encore valide.
    //
    // Même souci, de façon indirecte, avec les objets enfants (TwitchAuth
    // surtout, avec sa propre requête réseau possible en cours) : leur
    // destruction implicite par QObject n'intervient elle aussi qu'après ce
    // destructeur, et peut encore émettre un signal (authFailed, etc.) qui
    // atteindrait un de nos slots touchant ui. On coupe donc d'abord toute
    // connexion des enfants vers nous.
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

void MainWindow::onChatMessageReceived(const QString &username, const QString &message)
{
    ui->chatDisplay->append(
        QStringLiteral("<b>%1</b> : %2").arg(username.toHtmlEscaped(), message.toHtmlEscaped()));

    if (ui->ttsCheckBox->isChecked())
        m_tts->enqueue(message);
}

void MainWindow::onChatStatusChanged(const QString &status)
{
    // Déjà visible dans le panneau Chat lui-même (ligne ci-dessous) : la barre
    // de statut, elle, reste dédiée au message de marque permanent (cf.
    // constructeur) plutôt qu'à ces statuts transitoires.
    ui->chatDisplay->append(QStringLiteral("<i>%1</i>").arg(status.toHtmlEscaped()));
}

void MainWindow::onChatSendButtonClicked()
{
    const QString message = ui->chatSendEdit->text().trimmed();
    if (message.isEmpty() || !m_chat)
        return;

    m_chat->sendMessage(message);

    // Le serveur IRC de Twitch n'échoue pas nos propres messages : on simule
    // leur réception via le même chemin que n'importe quel autre message
    // (affichage + lecture TTS si activée), pour un comportement identique à
    // un message envoyé depuis Twitch directement.
    onChatMessageReceived(m_twitchAuth->login(), message);

    ui->chatSendEdit->clear();
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
    ui->twitchLoginButton->setText(tr("Déconnecter Twitch (%1)").arg(login));

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

    // La lecture du chat démarre automatiquement avec la chaîne authentifiée.
    // L'envoi de messages nécessite le scope chat:edit : une session plus
    // ancienne (connectée avant son ajout) ne l'a pas forcément encore -> on
    // se rabat sur une connexion anonyme (lecture seule, comme avant) plutôt
    // que de risquer une connexion IRC rejetée par Twitch.
    const bool canSendChat = m_twitchAuth->hasScope(QStringLiteral("chat:edit"));
    if (canSendChat)
        attachChat(new Chat(login, login, m_twitchAuth->accessToken(), this));
    else
        attachChat(new Chat(login, QString(), QString(), this));
    m_chat->connectToChat();

    ui->chatSendEdit->setEnabled(canSendChat);
    ui->chatSendButton->setEnabled(canSendChat);
    if (!canSendChat) {
        ui->chatDisplay->append(tr("<i>Reconnecte-toi à Twitch pour pouvoir écrire dans le chat "
                                    "(autorisation supplémentaire nécessaire).</i>"));
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
    ui->twitchLoginButton->setText(tr("Se connecter avec Twitch"));
    ui->twitchDisplayNameLabel->setVisible(false);
    ui->twitchAvatarLabel->setVisible(false);

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

    // Les présets ne servent qu'à remplir les champs ci-dessus, eux-mêmes
    // inutilisables hors connexion : on suit donc le même état. La liste
    // enregistrée localement (m_presets) n'est en revanche pas vidée, elle
    // reste disponible à la prochaine connexion.
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
    ui->streamStatusLabel->setText(tr("Échec du chargement : %1").arg(reason));
}

void MainWindow::onStreamUpdateSucceeded()
{
    ui->streamSaveButton->setEnabled(true);
    ui->streamStatusLabel->setText(tr("Enregistré sur Twitch."));
}

void MainWindow::onStreamUpdateFailed(const QString &reason)
{
    ui->streamSaveButton->setEnabled(true);
    ui->streamStatusLabel->setText(tr("Échec de l'enregistrement : %1").arg(reason));
}

void MainWindow::onStreamFetchButtonClicked()
{
    ui->streamStatusLabel->setText(tr("Chargement..."));
    m_twitchChannel->fetchInfo();
}

void MainWindow::onStreamSaveButtonClicked()
{
    const QString title = ui->streamTitleEdit->text().trimmed();
    if (title.isEmpty()) {
        ui->streamStatusLabel->setText(tr("Le titre ne peut pas être vide."));
        return;
    }

    // Les tags sont déjà validés (longueur, nombre) au moment de leur ajout
    // sous forme de chips : m_currentTags est directement utilisable.
    const QStringList &tags = m_currentTags;

    // Catégorie : laissée inchangée côté Twitch si le champ est vide ; sinon
    // il faut la résoudre en game_id parmi les noms déjà vus (chargement
    // initial + résultats de recherche), Twitch n'acceptant pas un nom brut.
    const QString categoryText = ui->streamCategoryEdit->text().trimmed();
    QString gameId;
    if (!categoryText.isEmpty()) {
        gameId = resolveCategoryId(categoryText);
        if (gameId.isEmpty()) {
            ui->streamStatusLabel->setText(
                tr("Catégorie \"%1\" non reconnue : choisis-la dans la liste proposée.").arg(categoryText));
            return;
        }
    }

    ui->streamSaveButton->setEnabled(false);
    ui->streamStatusLabel->setText(tr("Enregistrement..."));
    m_twitchChannel->updateInfo(title, gameId, tags);
}

void MainWindow::onStreamCategoryTextEdited(const QString &text)
{
    Q_UNUSED(text);

    // Anti-rebond : on ne lance la recherche Twitch qu'une fois la frappe
    // marquée une pause, pas à chaque caractère tapé.
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
        ui->streamStatusLabel->setText(tr("Le tag \"%1\" dépasse 25 caractères.").arg(tag));
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
        ui->streamStatusLabel->setText(tr("Le nom du préset ne peut pas être vide."));
        return;
    }

    StreamPreset preset;
    preset.name = name;
    preset.title = ui->streamTitleEdit->text();
    preset.gameName = ui->streamCategoryEdit->text().trimmed();
    preset.gameId = preset.gameName.isEmpty() ? QString() : resolveCategoryId(preset.gameName);
    preset.tags = m_currentTags;

    // Un préset du même nom (insensible à la casse) est mis à jour plutôt que
    // dupliqué.
    const int existingIndex = presetIndexByName(name);
    if (existingIndex >= 0) {
        m_presets[existingIndex] = preset;
    } else {
        if (m_presets.size() >= kMaxPresets) {
            ui->streamStatusLabel->setText(tr("%1 présets maximum.").arg(kMaxPresets));
            return;
        }
        m_presets.append(preset);
    }

    savePresets();
    refreshPresetList();
    ui->presetNameEdit->clear();
    ui->streamStatusLabel->setText(tr("Préset \"%1\" enregistré.").arg(name));
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

    // Si le préset connaît déjà le game_id de sa catégorie, on le réinjecte
    // pour qu'un "Enregistrer sur Twitch" immédiat n'ait pas besoin d'une
    // nouvelle recherche pour la résoudre.
    if (!preset.gameName.isEmpty() && !preset.gameId.isEmpty()) {
        m_categoryIdByName.insert(preset.gameName, preset.gameId);
        m_currentGameId = preset.gameId;
    }

    ui->streamStatusLabel->setText(tr("Préset \"%1\" chargé.").arg(preset.name));
}

void MainWindow::deletePreset(const QString &name)
{
    const int index = presetIndexByName(name);
    if (index < 0)
        return;

    // Suppression irréversible : demande confirmation, avec des boutons en
    // français (ceux d'un QMessageBox standard suivent la locale système,
    // souvent en anglais).
    QMessageBox confirm(this);
    confirm.setWindowTitle(tr("Supprimer le préset"));
    confirm.setText(tr("Supprimer le préset \"%1\" ? Cette action est irréversible.").arg(name));
    confirm.setIcon(QMessageBox::Warning);
    QPushButton *deleteButton = confirm.addButton(tr("Supprimer"), QMessageBox::DestructiveRole);
    confirm.addButton(tr("Annuler"), QMessageBox::RejectRole);
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

    // Copie de travail : les modifications ne touchent m_presets qu'une fois
    // validées (bouton "Enregistrer" de la micro pop-up).
    StreamPreset preset = m_presets.at(index);

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Préset : %1").arg(preset.name));

    auto *layout = new QVBoxLayout(&dialog);

    layout->addWidget(new QLabel(tr("Nom"), &dialog));
    auto *nameEdit = new QLineEdit(preset.name, &dialog);
    layout->addWidget(nameEdit);

    layout->addWidget(new QLabel(tr("Titre"), &dialog));
    auto *titleEdit = new QLineEdit(preset.title, &dialog);
    titleEdit->setMaxLength(140);
    layout->addWidget(titleEdit);

    layout->addWidget(new QLabel(tr("Catégorie"), &dialog));
    auto *categoryEdit = new QLineEdit(preset.gameName, &dialog);
    layout->addWidget(categoryEdit);

    layout->addWidget(new QLabel(tr("Tags (séparés par des virgules)"), &dialog));
    auto *tagsEdit = new QLineEdit(preset.tags.join(QStringLiteral(", ")), &dialog);
    layout->addWidget(tagsEdit);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    // QDialogButtonBox traduit ses boutons standards selon la locale système
    // (souvent en anglais ici) : on force un texte cohérent avec le reste de
    // l'interface, en français.
    buttonBox->button(QDialogButtonBox::Save)->setText(tr("Enregistrer"));
    buttonBox->button(QDialogButtonBox::Cancel)->setText(tr("Annuler"));
    layout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, [&]() {
        const QString newName = nameEdit->text().trimmed();
        if (newName.isEmpty()) {
            QMessageBox::warning(&dialog, tr("Préset"), tr("Le nom du préset ne peut pas être vide."));
            return;
        }
        const int conflictIndex = presetIndexByName(newName);
        if (conflictIndex >= 0 && conflictIndex != index) {
            QMessageBox::warning(&dialog, tr("Préset"),
                                  tr("Un préset nommé \"%1\" existe déjà.").arg(newName));
            return;
        }

        QStringList tags;
        const QStringList rawTags = tagsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &rawTag : rawTags) {
            const QString tag = rawTag.trimmed();
            if (tag.isEmpty())
                continue;
            if (tag.size() > 25) {
                QMessageBox::warning(&dialog, tr("Préset"),
                                      tr("Le tag \"%1\" dépasse 25 caractères.").arg(tag));
                return;
            }
            tags << tag;
        }
        if (tags.size() > 10) {
            QMessageBox::warning(&dialog, tr("Préset"), tr("10 tags maximum (%1 fournis).").arg(tags.size()));
            return;
        }

        const QString newGameName = categoryEdit->text().trimmed();
        preset.name = newName;
        preset.title = titleEdit->text();
        preset.gameName = newGameName;
        // Catégorie inchangée : on garde le game_id déjà connu plutôt que de
        // le perdre (une nouvelle résolution nécessiterait une recherche).
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
    // Un simple QVBoxLayout dans une QScrollArea plutôt qu'un QListWidget +
    // setItemWidget() : ce dernier ne recalculait la largeur de chaque ligne
    // (et donc la position de la croix de suppression, tout à droite) qu'au
    // premier redimensionnement réel du widget - jamais au remplissage
    // initial, même différé. Un layout "normal" comme celui-ci est géré par
    // le mécanisme de layout standard de Qt et n'a pas ce défaut.
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
        infoButton->setToolTip(tr("Voir / modifier les infos du préset"));
        infoButton->setStyleSheet(buttonStyle);
        connect(infoButton, &QToolButton::clicked, this, [this, name]() { showPresetInfoDialog(name); });
        rowLayout->addWidget(infoButton);

        auto *loadButton = new QToolButton(row);
        loadButton->setText(tr("Charger"));
        loadButton->setStyleSheet(buttonStyle);
        connect(loadButton, &QToolButton::clicked, this, [this, name]() { loadPreset(name); });
        rowLayout->addWidget(loadButton);

        // Croix de suppression, même style que sur les tags.
        auto *deleteButton = new QToolButton(row);
        deleteButton->setText(QStringLiteral("×"));
        deleteButton->setToolTip(tr("Supprimer le préset"));
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
