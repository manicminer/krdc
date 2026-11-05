/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include "avdendpoints.h"
#include "avdfeed.h"
#include "avdoauth.h"
#include "connectionlaunch.h"

#include <QDateTime>
#include <QDialog>
#include <QElapsedTimer>
#include <QPalette>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>

class AvdResourceFilter;
class AvdResourceModel;
class AvdResourceView;
class KMessageWidget;
class QAction;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QPushButton;
class QStackedWidget;
class QTabBar;

namespace Avd
{
class FeedClient;
class TokenClient;
class TokenStore;
class WorkspaceCache;
}

/**
 * Lists the desktops and applications of the user's Azure Virtual Desktop
 * workspaces as cards and opens the desktop the user picks. Nothing is
 * opened until the user does so.
 *
 * Lists come from the WorkspaceCache while they are younger than an hour, so
 * reopening the window needs neither the network nor the keychain. Opening a
 * desktop always gets a valid token first.
 */
class AvdWorkspaceDialog : public QDialog
{
    Q_OBJECT

public:
    /** Returns the answer the browser handed back for @p request, or nothing if canceled. */
    using InteractiveSignIn = std::function<std::optional<Avd::AuthorizationCallback>(const Avd::AuthorizationRequest &request)>;

    /** Without a @p cache, the dialog keeps its own, in memory. */
    AvdWorkspaceDialog(QNetworkAccessManager *network,
                       Avd::TokenStore *tokenStore,
                       const Avd::CloudEndpoints &endpoints,
                       QWidget *parent = nullptr,
                       Avd::WorkspaceCache *cache = nullptr);
    ~AvdWorkspaceDialog() override;

    void setInteractiveSignIn(InteractiveSignIn signIn);

    /** The account signed in last, used for silent sign-in and as login hint. */
    void setAccount(const QString &account);
    QString account() const;

    /** Shows the cached list, and signs in silently to update it if it is older than an hour. */
    void start();

    /** Signs in through the browser. */
    void signIn();
    /** Forgets the stored refresh token and the cached list of the account. */
    void signOut();
    /** Fetches the list again, whatever the age of the cached one. */
    void refresh();

    /** Opens the selected desktop. */
    void connectSelected();

    AvdResourceView *resourceView() const;
    AvdResourceModel *resourceModel() const;
    QTabBar *categoryBar() const;
    QLineEdit *searchField() const;
    QString statusText() const;
    /** What is shown in place of the cards, or an empty string while cards are shown. */
    QString placeholderText() const;
    /** The cards shown are from a fetch an hour or more ago. */
    bool isShowingStaleList() const;

protected:
    void changeEvent(QEvent *event) override;

Q_SIGNALS:
    void accountChanged(const QString &account);
    void launchRequested(const ConnectionLaunch &launch);

private:
    enum class State {
        SignedOut,
        Loading,
        Loaded,
        Failed,
    };

    void setBusy(const QString &message);
    void showError(const QString &message);
    void acquireToken();
    void continueWithToken();
    bool hasValidToken() const;
    void discover();
    void handleToken(const Avd::TokenResponse &token);
    void handleResources(const QList<Avd::Resource> &resources, const QStringList &warnings, bool complete);
    void showList(const QList<Avd::Resource> &resources, const QHash<QString, QImage> &icons, const QDateTime &fetchedAt, const QStringList &warnings);
    void clearList();
    void open(const QModelIndex &index);
    void launch(const Avd::Resource &resource, const QByteArray &contents);
    void updateCategories();
    void updatePage();
    void updateStatus(const QString &message = QString());
    void updateButtons();
    void updateIcons();
    QIcon controlIcon(const QString &name, QPalette::ColorRole background, QPalette::ColorRole foreground, const QString &fallback = QString()) const;

    Avd::CloudEndpoints m_endpoints;
    Avd::TokenStore *m_tokenStore;
    std::unique_ptr<Avd::WorkspaceCache> m_ownCache;
    Avd::WorkspaceCache *m_cache;
    Avd::FeedClient *m_feed;
    Avd::TokenClient *m_tokens;
    InteractiveSignIn m_interactive;
    QString m_account;
    QString m_accessToken;
    QDateTime m_accessTokenReceived;
    QElapsedTimer m_accessTokenTimer;
    qint64 m_accessTokenLifetime = 0;
    Avd::AuthorizationRequest m_pendingRequest;
    /** Key of the desktop to open once a token or a fresh feed is there. */
    QString m_pendingLaunch;
    QDateTime m_listFetchedAt;
    State m_state = State::SignedOut;
    QString m_error;
    bool m_silentAttempt = false;
    bool m_busy = false;
    /** Changes with every sign-in, sign-out and account change. */
    quint64 m_authGeneration = 0;
    quint64 m_discoverGeneration = 0;
    QString m_discoverAccount;
    /** The cache's generation of the account when the fetch began; see Avd::WorkspaceCache::generation(). */
    quint64 m_discoverCacheGeneration = 0;

    AvdResourceModel *m_model;
    AvdResourceFilter *m_filter;
    AvdResourceView *m_view;
    QLabel *m_accountLabel;
    QLineEdit *m_search;
    QAction *m_searchAction = nullptr;
    QTabBar *m_categories;
    KMessageWidget *m_message;
    QStackedWidget *m_pages;
    QWidget *m_placeholder;
    QLabel *m_placeholderIcon;
    QLabel *m_placeholderLabel;
    QPushButton *m_placeholderButton;
    QLabel *m_status;
    QPushButton *m_signInButton;
    QPushButton *m_signOutButton;
    QPushButton *m_refreshButton;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_closeButton = nullptr;
    bool m_closeButtonHasIcon = false;
};
