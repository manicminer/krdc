/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdworkspacedialog.h"

#include "avd/avdfeedclient.h"
#include "avd/avdresourceview.h"
#include "avd/avdsignindialog.h"
#include "avd/avdtokenstore.h"
#include "avd/avdworkspacecache.h"
#include "rdplaunchprofile.h"

#include <KLocalizedString>
#include <KMessageWidget>

#include <QAction>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QVBoxLayout>

namespace
{
// Tokens are renewed this long before they expire, so a launch does not hand over one about to lapse.
constexpr qint64 TokenMargin = 300;
constexpr int MaxIconDownloads = 200;

qreal luminance(const QColor &color)
{
    return 0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF();
}

/**
 * The theme icon, or, if it is a single-coloured glyph barely visible on @p background, the same
 * glyph in @p foreground. Plasma recolours symbolic icons itself; other platforms, and Breeze
 * icons under a dark scheme without the KDE platform theme, do not. Colourful icons are kept.
 */
QIcon contrastingIcon(const QIcon &icon, const QColor &background, const QColor &foreground)
{
    if (icon.isNull()) {
        return icon;
    }
    const QImage sample = icon.pixmap(22, 22).toImage().convertToFormat(QImage::Format_ARGB32);
    qreal weight = 0;
    qreal light = 0;
    qreal saturation = 0;
    for (int y = 0; y < sample.height(); ++y) {
        for (int x = 0; x < sample.width(); ++x) {
            const QColor color = sample.pixelColor(x, y);
            weight += color.alphaF();
            light += color.alphaF() * luminance(color);
            saturation += color.alphaF() * color.hsvSaturationF();
        }
    }
    if (weight == 0) {
        return icon;
    }
    const qreal glyph = light / weight;
    const qreal back = luminance(background);
    const qreal contrast = (std::max(glyph, back) + 0.05) / (std::min(glyph, back) + 0.05);
    // 3:1 is what WCAG asks of user interface graphics.
    if (contrast >= 3 || saturation / weight > 0.25) {
        return icon;
    }
    QIcon recoloured;
    for (int size : {16, 22, 32, 48, 64}) {
        QPixmap pixmap = icon.pixmap(size, size);
        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), foreground);
        painter.end();
        recoloured.addPixmap(pixmap);
    }
    return recoloured;
}

QString formatTime(const QDateTime &time, const QDateTime &now)
{
    const QDateTime local = time.toLocalTime();
    if (local.date() == now.toLocalTime().date()) {
        return QLocale().toString(local.time(), QLocale::ShortFormat);
    }
    return QLocale().toString(local, QLocale::ShortFormat);
}
}

AvdWorkspaceDialog::AvdWorkspaceDialog(QNetworkAccessManager *network,
                                       Avd::TokenStore *tokenStore,
                                       const Avd::CloudEndpoints &endpoints,
                                       QWidget *parent,
                                       Avd::WorkspaceCache *cache)
    : QDialog(parent)
    , m_endpoints(endpoints)
    , m_tokenStore(tokenStore)
    , m_ownCache(cache ? nullptr : std::make_unique<Avd::WorkspaceCache>())
    , m_cache(cache ? cache : m_ownCache.get())
    , m_feed(new Avd::FeedClient(network, endpoints, this))
    , m_tokens(new Avd::TokenClient(network, endpoints, this))
    , m_model(new AvdResourceModel(this))
    , m_filter(new AvdResourceFilter(this))
{
    setWindowTitle(i18nc("@title:window", "Azure Virtual Desktop Workspaces"));
    resize(880, 640);

    m_interactive = [this](const Avd::AuthorizationRequest &request) -> std::optional<Avd::AuthorizationCallback> {
        // On the heap: closing the main window deletes this dialog, and its children with it, while exec() runs.
        QPointer<AvdSignInDialog> dialog =
            new AvdSignInDialog({request.url, request.redirectUri, request.state, i18nc("@info", "Sign in to list your Azure Virtual Desktop workspaces.")},
                                this);
        dialog->exec();
        if (!dialog) {
            return std::nullopt;
        }
        const std::optional<Avd::AuthorizationCallback> callback = dialog->callback();
        delete dialog;
        return callback;
    };

    auto *layout = new QVBoxLayout(this);

    // Header: title and account on the left, search and refresh on the right.
    auto *header = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(0);
    auto *title = new QLabel(i18nc("@title", "Azure Virtual Desktop"), this);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.6);
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    title->setAccessibleName(title->text());
    m_accountLabel = new QLabel(this);
    // The account, titles and search text come from elsewhere: never markup.
    m_accountLabel->setTextFormat(Qt::PlainText);
    m_accountLabel->setForegroundRole(QPalette::PlaceholderText);
    titles->addWidget(title);
    titles->addWidget(m_accountLabel);
    header->addLayout(titles, 1);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(i18nc("@info:placeholder", "Search"));
    m_search->setClearButtonEnabled(true);
    m_searchAction = m_search->addAction(QIcon(), QLineEdit::LeadingPosition);
    m_search->setAccessibleName(i18nc("@info accessible name", "Search desktops and apps"));
    m_search->setMinimumWidth(140);
    m_search->setMaximumWidth(260);
    header->addWidget(m_search, 0, Qt::AlignVCenter);
    m_refreshButton = new QPushButton(i18nc("@action:button", "Refresh"), this);
    m_refreshButton->setToolTip(i18nc("@info:tooltip", "Load the list of desktops and apps again"));
    m_refreshButton->setAutoDefault(false);
    header->addWidget(m_refreshButton, 0, Qt::AlignVCenter);
    layout->addLayout(header);

    m_categories = new QTabBar(this);
    m_categories->setDocumentMode(true);
    m_categories->setDrawBase(false);
    m_categories->setExpanding(false);
    m_categories->addTab(QString());
    m_categories->addTab(QString());
    m_categories->setAccessibleName(i18nc("@info accessible name", "Resource category"));
    layout->addWidget(m_categories);

    m_message = new KMessageWidget(this);
    m_message->setWordWrap(true);
    m_message->setCloseButtonVisible(true);
    m_message->hide();
    layout->addWidget(m_message);

    m_view = new AvdResourceView(this);
    m_filter->setSourceModel(m_model);
    m_view->setModel(m_filter);

    m_placeholder = new QWidget(this);
    auto *placeholderLayout = new QVBoxLayout(m_placeholder);
    placeholderLayout->addStretch();
    m_placeholderIcon = new QLabel(m_placeholder);
    m_placeholderIcon->setAlignment(Qt::AlignCenter);
    m_placeholderLabel = new QLabel(m_placeholder);
    m_placeholderLabel->setTextFormat(Qt::PlainText);
    m_placeholderLabel->setAlignment(Qt::AlignCenter);
    m_placeholderLabel->setWordWrap(true);
    m_placeholderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont placeholderFont = m_placeholderLabel->font();
    placeholderFont.setPointSizeF(placeholderFont.pointSizeF() * 1.15);
    m_placeholderLabel->setFont(placeholderFont);
    m_placeholderButton = new QPushButton(m_placeholder);
    m_placeholderButton->setAutoDefault(false);
    placeholderLayout->addWidget(m_placeholderIcon);
    placeholderLayout->addWidget(m_placeholderLabel);
    placeholderLayout->addWidget(m_placeholderButton, 0, Qt::AlignCenter);
    placeholderLayout->addStretch();

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(m_view);
    m_pages->addWidget(m_placeholder);
    layout->addWidget(m_pages, 1);

    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);

    auto *notice = new QLabel(i18nc("@info",
                                    "KRDC is not a Microsoft client. To list workspaces it identifies itself as Microsoft's "
                                    "Remote Desktop web client, which Microsoft may stop accepting at any time. "
                                    "Only full desktops can be opened so far."),
                              this);
    notice->setWordWrap(true);
    notice->setForegroundRole(QPalette::PlaceholderText);
    QFont noticeFont = notice->font();
    noticeFont.setPointSizeF(noticeFont.pointSizeF() * 0.9);
    notice->setFont(noticeFont);
    layout->addWidget(notice);

    auto *buttons = new QHBoxLayout;
    m_signInButton = new QPushButton(i18nc("@action:button", "Sign In…"), this);
    m_signOutButton = new QPushButton(i18nc("@action:button", "Sign Out"), this);
    m_connectButton = new QPushButton(i18nc("@action:button", "Connect"), this);
    m_connectButton->setDefault(true);
    m_signInButton->setAutoDefault(false);
    m_signOutButton->setAutoDefault(false);
    buttons->addWidget(m_signInButton);
    buttons->addWidget(m_signOutButton);
    buttons->addStretch();
    buttons->addWidget(m_connectButton);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_closeButton = close->button(QDialogButtonBox::Close);
    // Only styles that put icons on dialog buttons get one back after a palette change.
    m_closeButtonHasIcon = !m_closeButton->icon().isNull();
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(m_signInButton, &QPushButton::clicked, this, &AvdWorkspaceDialog::signIn);
    connect(m_signOutButton, &QPushButton::clicked, this, &AvdWorkspaceDialog::signOut);
    connect(m_refreshButton, &QPushButton::clicked, this, &AvdWorkspaceDialog::refresh);
    connect(m_connectButton, &QPushButton::clicked, this, &AvdWorkspaceDialog::connectSelected);
    connect(m_placeholderButton, &QPushButton::clicked, this, [this]() {
        if (m_state == State::SignedOut) {
            signIn();
        } else {
            refresh();
        }
    });
    connect(m_view, &QAbstractItemView::activated, this, &AvdWorkspaceDialog::open);
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this, &AvdWorkspaceDialog::updateButtons);
    connect(m_categories, &QTabBar::currentChanged, this, [this](int index) {
        const auto category = index == 1 ? AvdResourceModel::Category::Apps : AvdResourceModel::Category::Devices;
        m_filter->setCategory(category);
        m_view->setCategory(category);
        updatePage();
        updateButtons();
    });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_filter->setSearchText(text);
        updatePage();
        updateButtons();
    });

    connect(m_tokens, &Avd::TokenClient::tokenReceived, this, &AvdWorkspaceDialog::handleToken);
    connect(m_tokens, &Avd::TokenClient::failed, this, [this](const QString &message) {
        m_pendingLaunch.clear();
        if (m_silentAttempt) {
            // The stored sign-in expired or was revoked; it is of no further use.
            m_silentAttempt = false;
            if (!m_account.isEmpty()) {
                m_tokenStore->remove(m_account);
            }
            showError(i18nc("@info", "Sign in again to update your workspaces. (%1)", message));
            return;
        }
        showError(message);
    });
    connect(m_feed, &Avd::FeedClient::resourcesReady, this, &AvdWorkspaceDialog::handleResources);
    connect(m_feed, &Avd::FeedClient::connectionFileReady, this, &AvdWorkspaceDialog::launch);
    connect(m_feed, &Avd::FeedClient::iconReady, this, [this](const QString &key, const QImage &icon) {
        if (m_discoverGeneration != m_authGeneration || m_discoverAccount != m_account) {
            return;
        }
        m_cache->storeIcon(m_endpoints, m_account, key, icon, m_discoverCacheGeneration);
        m_model->setIcon(key, icon);
    });
    connect(m_feed, &Avd::FeedClient::failed, this, [this](const QString &message) {
        m_pendingLaunch.clear();
        showError(message);
    });

    updateIcons();
    updateCategories();
    updatePage();
    updateStatus();
    updateButtons();
}

AvdWorkspaceDialog::~AvdWorkspaceDialog()
{
    m_feed->abort();
    m_tokens->abort();
}

void AvdWorkspaceDialog::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (!m_connectButton) {
        // Still being built.
        return;
    }
    switch (event->type()) {
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
    case QEvent::ThemeChange:
        updateIcons();
        updatePage();
        break;
    default:
        break;
    }
}

QIcon AvdWorkspaceDialog::controlIcon(const QString &name, QPalette::ColorRole background, QPalette::ColorRole foreground, const QString &fallback) const
{
    QIcon icon = QIcon::fromTheme(name);
    if (icon.isNull() && !fallback.isEmpty()) {
        icon = QIcon::fromTheme(fallback);
    }
    return contrastingIcon(icon, palette().color(background), palette().color(foreground));
}

void AvdWorkspaceDialog::updateIcons()
{
    m_searchAction->setIcon(controlIcon(QStringLiteral("search"), QPalette::Base, QPalette::Text));
    m_refreshButton->setIcon(controlIcon(QStringLiteral("view-refresh"), QPalette::Button, QPalette::ButtonText));
    m_connectButton->setIcon(controlIcon(QStringLiteral("network-connect"), QPalette::Button, QPalette::ButtonText));
    if (m_closeButtonHasIcon) {
        m_closeButton->setIcon(contrastingIcon(style()->standardIcon(QStyle::SP_DialogCloseButton, nullptr, m_closeButton),
                                               palette().color(QPalette::Button),
                                               palette().color(QPalette::ButtonText)));
    }
    m_categories->setTabIcon(0, controlIcon(QStringLiteral("computer"), QPalette::Window, QPalette::WindowText));
    m_categories->setTabIcon(
        1,
        controlIcon(QStringLiteral("applications-all-symbolic"), QPalette::Window, QPalette::WindowText, QStringLiteral("applications-other")));
}

void AvdWorkspaceDialog::setInteractiveSignIn(InteractiveSignIn signIn)
{
    m_interactive = std::move(signIn);
}

void AvdWorkspaceDialog::setAccount(const QString &account)
{
    ++m_authGeneration;
    if (account != m_account) {
        // Nothing of the previous account may carry over: neither its token nor its list.
        m_feed->abort();
        m_tokens->abort();
        m_silentAttempt = false;
        m_accessToken.clear();
        m_pendingLaunch.clear();
        clearList();
    }
    m_account = account;
    m_state = account.isEmpty() ? State::SignedOut : State::Loaded;
    updatePage();
    updateStatus();
    updateButtons();
}

QString AvdWorkspaceDialog::account() const
{
    return m_account;
}

AvdResourceView *AvdWorkspaceDialog::resourceView() const
{
    return m_view;
}

AvdResourceModel *AvdWorkspaceDialog::resourceModel() const
{
    return m_model;
}

QTabBar *AvdWorkspaceDialog::categoryBar() const
{
    return m_categories;
}

QLineEdit *AvdWorkspaceDialog::searchField() const
{
    return m_search;
}

QString AvdWorkspaceDialog::statusText() const
{
    return m_status->text();
}

QString AvdWorkspaceDialog::placeholderText() const
{
    return m_pages->currentWidget() == m_placeholder ? m_placeholderLabel->text() : QString();
}

bool AvdWorkspaceDialog::isShowingStaleList() const
{
    if (!m_listFetchedAt.isValid()) {
        return false;
    }
    Avd::WorkspaceCache::Entry entry;
    entry.fetchedAt = m_listFetchedAt;
    return !m_cache->isFresh(entry);
}

void AvdWorkspaceDialog::start()
{
    if (m_account.isEmpty()) {
        return;
    }
    if (const auto entry = m_cache->lookup(m_endpoints, m_account)) {
        showList(entry->resources, entry->icons, entry->fetchedAt, entry->warnings);
        if (m_cache->isFresh(*entry)) {
            return;
        }
    }
    acquireToken();
}

void AvdWorkspaceDialog::refresh()
{
    if (m_account.isEmpty() && m_accessToken.isEmpty()) {
        return;
    }
    m_pendingLaunch.clear();
    acquireToken();
}

bool AvdWorkspaceDialog::hasValidToken() const
{
    if (m_accessToken.isEmpty() || !m_accessTokenTimer.isValid()) {
        return false;
    }
    // Both clocks must agree: the monotonic one is immune to the wall clock being set back, the wall
    // clock (which tests can set) also counts time the machine was suspended.
    const QDateTime now = m_cache->now();
    return m_accessTokenTimer.elapsed() < m_accessTokenLifetime * 1000 && now >= m_accessTokenReceived
        && m_accessTokenReceived.secsTo(now) < m_accessTokenLifetime;
}

void AvdWorkspaceDialog::acquireToken()
{
    if (hasValidToken()) {
        continueWithToken();
        return;
    }
    if (m_account.isEmpty()) {
        m_pendingLaunch.clear();
        m_state = State::SignedOut;
        updatePage();
        updateStatus(i18nc("@info:status", "Sign in to list your workspaces."));
        updateButtons();
        return;
    }
    setBusy(i18nc("@info:status", "Signing in as %1…", m_account));
    QPointer<AvdWorkspaceDialog> self(this);
    const quint64 generation = m_authGeneration;
    const QString account = m_account;
    m_tokenStore->read(m_account, [self, generation, account](const QString &refreshToken) {
        // The keychain may answer after the user signed out or switched accounts.
        if (!self || self->m_authGeneration != generation || self->m_account != account) {
            return;
        }
        if (refreshToken.isEmpty()) {
            self->m_pendingLaunch.clear();
            self->m_busy = false;
            if (self->m_model->rowCount() == 0) {
                self->m_state = State::SignedOut;
            }
            self->updatePage();
            self->updateStatus(i18nc("@info:status", "Sign in to update your workspaces."));
            self->updateButtons();
            return;
        }
        self->m_silentAttempt = true;
        self->m_tokens->refresh(refreshToken);
    });
}

void AvdWorkspaceDialog::signIn()
{
    const quint64 generation = ++m_authGeneration;
    m_pendingRequest = Avd::createAuthorizationRequest(m_endpoints, m_account);
    QPointer<AvdWorkspaceDialog> self(this);
    // Copies: this dialog, with its members, may be destroyed while the sign-in runs.
    const InteractiveSignIn interactive = m_interactive;
    const Avd::AuthorizationRequest request = m_pendingRequest;
    const std::optional<Avd::AuthorizationCallback> callback = interactive ? interactive(request) : std::nullopt;
    if (!self || m_authGeneration != generation) {
        // Signed out, switched accounts or started over meanwhile: this answer is of no use.
        return;
    }
    if (!callback) {
        updateStatus(i18nc("@info:status", "Sign-in canceled."));
        return;
    }
    QString error;
    const auto code = Avd::authorizationCodeFromCallback(*callback, m_pendingRequest, &error);
    if (!code) {
        showError(error);
        return;
    }
    m_silentAttempt = false;
    setBusy(i18nc("@info:status", "Signing in…"));
    m_tokens->redeemCode(*code, m_pendingRequest);
}

void AvdWorkspaceDialog::signOut()
{
    ++m_authGeneration;
    m_silentAttempt = false;
    m_busy = false;
    m_feed->abort();
    m_tokens->abort();
    if (!m_account.isEmpty()) {
        m_tokenStore->remove(m_account);
        m_cache->remove(m_endpoints, m_account);
    }
    m_account.clear();
    m_accessToken.clear();
    m_pendingLaunch.clear();
    clearList();
    m_state = State::SignedOut;
    updatePage();
    updateStatus(i18nc("@info:status", "Signed out."));
    Q_EMIT accountChanged(QString());
    updateButtons();
}

void AvdWorkspaceDialog::handleToken(const Avd::TokenResponse &token)
{
    m_silentAttempt = false;
    m_accessToken = token.accessToken;
    const qint64 lifetime = token.expiresIn > TokenMargin ? token.expiresIn - TokenMargin : std::min<qint64>(std::max<qint64>(token.expiresIn, 0), TokenMargin);
    m_accessTokenLifetime = lifetime;
    m_accessTokenReceived = m_cache->now();
    m_accessTokenTimer.start();
    if (!token.account.isEmpty() && token.account != m_account) {
        if (!m_account.isEmpty()) {
            clearList();
        }
        m_account = token.account;
        Q_EMIT accountChanged(m_account);
    }
    if (!token.refreshToken.isEmpty() && !m_account.isEmpty()) {
        m_tokenStore->write(m_account, token.refreshToken);
    }
    continueWithToken();
}

void AvdWorkspaceDialog::continueWithToken()
{
    if (!m_pendingLaunch.isEmpty()) {
        const auto resource = m_model->resource(m_pendingLaunch);
        if (resource && resource->isLaunchable()) {
            setBusy(i18nc("@info:status", "Opening “%1”…", resource->title));
            m_feed->downloadConnectionFile(*resource, m_accessToken);
            return;
        }
    }
    // Lists read from disk have no addresses; a fresh feed has them.
    discover();
}

void AvdWorkspaceDialog::discover()
{
    m_discoverGeneration = m_authGeneration;
    m_discoverAccount = m_account;
    m_discoverCacheGeneration = m_cache->generation(m_endpoints, m_account);
    setBusy(m_pendingLaunch.isEmpty() ? i18nc("@info:status", "Loading workspaces…") : i18nc("@info:status", "Finding the desktop…"));
    m_feed->discover(m_accessToken);
}

void AvdWorkspaceDialog::handleResources(const QList<Avd::Resource> &resources, const QStringList &warnings, bool complete)
{
    if (m_discoverGeneration != m_authGeneration || m_discoverAccount != m_account) {
        return;
    }
    m_busy = false;
    QHash<QString, QImage> icons;
    if (complete) {
        // Not stored if the account was signed out meanwhile, in this window or another one.
        if (m_cache->store(m_endpoints, m_account, resources, warnings, m_discoverCacheGeneration)) {
            if (const auto entry = m_cache->lookup(m_endpoints, m_account)) {
                icons = entry->icons;
            }
        }
        showList(resources, icons, m_cache->now(), warnings);
    } else if (m_model->rowCount() == 0) {
        showList(resources, icons, m_cache->now(), warnings);
    } else {
        // An incomplete answer replaces neither the cards nor the cache.
        m_message->setMessageType(KMessageWidget::Warning);
        m_message->setText(
            i18nc("@info", "The list could not be updated completely; the cards are from the last complete update.\n%1", warnings.join(QLatin1Char('\n'))));
        m_message->animatedShow();
        updateStatus();
        updateButtons();
    }

    if (!m_pendingLaunch.isEmpty()) {
        const QString key = std::exchange(m_pendingLaunch, QString());
        const auto found = std::find_if(resources.cbegin(), resources.cend(), [&key](const Avd::Resource &resource) {
            return resource.key() == key;
        });
        if (found != resources.cend() && found->isLaunchable()) {
            setBusy(i18nc("@info:status", "Opening “%1”…", found->title));
            m_feed->downloadConnectionFile(*found, m_accessToken);
        } else {
            showError(i18nc("@info", "This desktop is no longer in your workspaces."));
        }
    }

    int downloads = 0;
    for (const Avd::Resource &resource : resources) {
        if (downloads >= MaxIconDownloads) {
            break;
        }
        if (!resource.iconUrl.isEmpty() && !icons.contains(resource.key())) {
            m_feed->downloadIcon(resource, m_accessToken);
            ++downloads;
        }
    }
}

void AvdWorkspaceDialog::showList(const QList<Avd::Resource> &resources,
                                  const QHash<QString, QImage> &icons,
                                  const QDateTime &fetchedAt,
                                  const QStringList &warnings)
{
    const QString current = m_view->currentIndex().data(AvdResourceModel::KeyRole).toString();
    m_model->setResources(resources, icons);
    m_listFetchedAt = fetchedAt;
    m_state = State::Loaded;
    m_error.clear();
    if (warnings.isEmpty()) {
        m_message->hide();
    } else {
        m_message->setMessageType(KMessageWidget::Warning);
        m_message->setText(warnings.join(QLatin1Char('\n')));
        m_message->show();
    }
    if (m_model->count(AvdResourceModel::Category::Devices) == 0 && m_model->count(AvdResourceModel::Category::Apps) > 0) {
        m_categories->setCurrentIndex(1);
    }
    for (int row = 0; row < m_filter->rowCount(); ++row) {
        const QModelIndex index = m_filter->index(row, 0);
        if (index.data(AvdResourceModel::KeyRole).toString() == current) {
            m_view->setCurrentIndex(index);
        }
    }
    updateCategories();
    updatePage();
    updateStatus();
    updateButtons();
}

void AvdWorkspaceDialog::clearList()
{
    m_model->clear();
    m_listFetchedAt = QDateTime();
    m_message->hide();
    updateCategories();
}

void AvdWorkspaceDialog::connectSelected()
{
    open(m_view->currentIndex());
}

void AvdWorkspaceDialog::open(const QModelIndex &index)
{
    if (!index.isValid() || m_busy) {
        return;
    }
    const auto resource = m_model->resource(index.data(AvdResourceModel::KeyRole).toString());
    if (!resource) {
        return;
    }
    if (!AvdResourceModel::canOpen(*resource)) {
        updateStatus(resource->type == Avd::Resource::Type::RemoteApp
                         ? i18nc("@info:status", "KRDC cannot open remote applications such as “%1” yet; support is coming soon.", resource->title)
                         : i18nc("@info:status", "KRDC cannot open “%1”.", resource->title));
        return;
    }
    m_pendingLaunch = resource->key();
    setBusy(i18nc("@info:status", "Opening “%1”…", resource->title));
    acquireToken();
}

void AvdWorkspaceDialog::launch(const Avd::Resource &resource, const QByteArray &contents)
{
    m_busy = false;
    m_pendingLaunch.clear();
    QString error;
    auto profile = RdpLaunchProfile::fromContents(contents, RdpLaunchProfile::Source::AvdWorkspace, &error);
    if (!profile) {
        showError(error);
        return;
    }
    if (profile->isRemoteApp()) {
        showError(i18nc("@info", "“%1” is a remote application. KRDC can only open full desktops so far.", resource.title));
        return;
    }
    if (!profile->setGatewayBearerToken(m_accessToken, m_endpoints)) {
        showError(i18nc("@info", "“%1” does not connect through an Azure Virtual Desktop gateway. KRDC does not open it.", resource.title));
        return;
    }
    const ConnectionLaunch launch = rdpLaunchFromProfile(std::make_shared<const RdpLaunchProfile>(std::move(*profile)), resource.title);
    updateStatus(i18nc("@info:status", "Opened “%1”.", resource.title));
    updateButtons();
    Q_EMIT launchRequested(launch);
    accept();
}

void AvdWorkspaceDialog::setBusy(const QString &message)
{
    m_busy = true;
    if (m_model->rowCount() == 0) {
        m_state = State::Loading;
    }
    updatePage();
    updateStatus(message);
    updateButtons();
}

void AvdWorkspaceDialog::showError(const QString &message)
{
    m_busy = false;
    if (m_model->rowCount() > 0) {
        // Keep the cards; say plainly that they were not updated.
        m_message->setMessageType(KMessageWidget::Error);
        m_message->setText(message);
        m_message->animatedShow();
    } else {
        m_state = State::Failed;
        m_error = message;
    }
    updatePage();
    updateStatus();
    updateButtons();
}

void AvdWorkspaceDialog::updateCategories()
{
    m_categories->setTabText(0, i18nc("@title:tab remote desktops, with their number", "Devices (%1)", m_model->count(AvdResourceModel::Category::Devices)));
    m_categories->setTabText(1, i18nc("@title:tab remote applications, with their number", "Apps (%1)", m_model->count(AvdResourceModel::Category::Apps)));
    m_categories->setTabToolTip(1, i18nc("@info:tooltip", "Remote applications are listed; opening them is coming soon."));
    m_accountLabel->setText(m_account.isEmpty() ? i18nc("@info", "Not signed in") : i18nc("@info", "Signed in as %1", m_account));
}

void AvdWorkspaceDialog::updatePage()
{
    QString text;
    QString button;
    QString icon = QStringLiteral("cloud");
    if (m_model->rowCount() == 0) {
        switch (m_state) {
        case State::SignedOut:
            text = i18nc("@info", "Sign in to see your desktops and apps.");
            button = i18nc("@action:button", "Sign In…");
            icon = QStringLiteral("im-user");
            break;
        case State::Loading:
            text = i18nc("@info", "Loading your workspaces…");
            icon = QStringLiteral("view-refresh");
            break;
        case State::Failed:
            text = m_error;
            button = i18nc("@action:button", "Try Again");
            icon = QStringLiteral("dialog-warning");
            break;
        case State::Loaded:
            text = m_listFetchedAt.isValid() ? i18nc("@info", "This account has no Azure Virtual Desktop resources.")
                                             : i18nc("@info", "Refresh to list your workspaces.");
            button = i18nc("@action:button", "Refresh");
            break;
        }
    } else if (m_filter->rowCount() == 0) {
        icon = QStringLiteral("edit-find");
        if (!m_search->text().trimmed().isEmpty()) {
            text = i18nc("@info", "Nothing matches “%1”.", m_search->text().trimmed());
        } else if (m_filter->category() == AvdResourceModel::Category::Devices) {
            text = i18nc("@info", "No desktops in your workspaces. Remote applications are listed under Apps.");
            icon = QStringLiteral("computer");
        } else {
            text = i18nc("@info", "No remote applications in your workspaces.");
        }
    }

    if (text.isEmpty()) {
        m_pages->setCurrentWidget(m_view);
        return;
    }
    m_placeholderIcon->setPixmap(controlIcon(icon, QPalette::Window, QPalette::WindowText).pixmap(64, 64));
    m_placeholderLabel->setText(text);
    m_placeholderButton->setText(button);
    m_placeholderButton->setVisible(!button.isEmpty());
    m_pages->setCurrentWidget(m_placeholder);
}

void AvdWorkspaceDialog::updateStatus(const QString &message)
{
    QStringList lines;
    if (!message.isEmpty()) {
        lines << message;
    }
    if (m_listFetchedAt.isValid()) {
        const QString time = formatTime(m_listFetchedAt, m_cache->now());
        lines << (isShowingStaleList() ? i18nc("@info:status", "Showing the list from %1; it may be out of date.", time)
                                       : i18nc("@info:status", "List updated at %1.", time));
    }
    m_status->setText(lines.join(QLatin1Char(' ')));
    updateCategories();
}

void AvdWorkspaceDialog::updateButtons()
{
    const QModelIndex current = m_view->currentIndex();
    const bool signedIn = !m_account.isEmpty() || !m_accessToken.isEmpty();
    m_connectButton->setEnabled(!m_busy && signedIn && current.isValid() && current.data(AvdResourceModel::LaunchableRole).toBool());
    m_signOutButton->setEnabled(!m_account.isEmpty());
    m_refreshButton->setEnabled(!m_busy && signedIn);
}
