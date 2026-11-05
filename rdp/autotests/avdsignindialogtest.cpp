/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdsignindialog.h"
#include "avd/avdcallbackchannel.h"
#include "avd/avdresourceview.h"
#include "avd/avdtokenstore.h"
#include "avd/avdworkspacedialog.h"
#include "fakenetwork.h"
#include "rdplaunchprofile.h"

#include <KApplicationTrader>
#include <KService>
#include <KSycoca>

#include <QDialogButtonBox>
#include <QDirIterator>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

using namespace Avd;

namespace
{
const QUrl DiscoveryUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feeddiscovery"));
const QUrl FeedUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feed/webfeed?tenant=t1"));
const QUrl DesktopRdpUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feed/prod/rdp/desktop-1.rdp"));

const QByteArray Discovery =
    "<TenantFeedURLs><TenantFeedURL FeedURL=\"https://rdweb.wvd.microsoft.com/api/arm/feed/webfeed?tenant=t1\" TenantId=\"t1\" "
    "TenantDisplayName=\"Example Org\"/></TenantFeedURLs>";
const QByteArray Feed =
    "<ResourceCollection><Publisher Name=\"Example Workspace\"><Resources>"
    "<Resource ID=\"desktop-1\" Title=\"Example Desktop\" Type=\"Desktop\"><HostingTerminalServers><HostingTerminalServer>"
    "<ResourceFile FileExtension=\".rdp\" URL=\"https://rdweb.wvd.microsoft.com/api/arm/feed/prod/rdp/desktop-1.rdp\"/>"
    "</HostingTerminalServer></HostingTerminalServers></Resource>"
    "<Resource ID=\"app-1\" Title=\"Example App\" Type=\"RemoteApp\"><HostingTerminalServers><HostingTerminalServer>"
    "<ResourceFile FileExtension=\".rdp\" URL=\"https://rdweb.wvd.microsoft.com/api/arm/feed/prod/rdp/app-1.rdp\"/>"
    "</HostingTerminalServer></HostingTerminalServers></Resource>"
    "</Resources></Publisher></ResourceCollection>";
const QByteArray DesktopRdp =
    "full address:s:hostpool.example.org\r\ngatewayhostname:s:rdgateway.wvd.microsoft.com\r\nenablerdsaadauth:i:1\r\nresourceprovider:s:arm\r\n";

class MemoryTokenStore : public TokenStore
{
public:
    void read(const QString &account, ReadCallback callback) override
    {
        callback(tokens.value(account));
    }
    void write(const QString &account, const QString &refreshToken) override
    {
        tokens.insert(account, refreshToken);
    }
    void remove(const QString &account) override
    {
        tokens.remove(account);
    }
    QHash<QString, QString> tokens;
};

// Answers reads only when told to, as the system keychain does.
class DeferredTokenStore : public MemoryTokenStore
{
public:
    void read(const QString &account, ReadCallback callback) override
    {
        pending.append({account, std::move(callback)});
    }
    void completeReads()
    {
        const auto reads = std::exchange(pending, {});
        for (const auto &[account, callback] : reads) {
            callback(tokens.value(account, QStringLiteral("stored-for-") + account));
        }
    }
    QList<std::pair<QString, ReadCallback>> pending;
};

QByteArray tokenJson(const QString &access, const QString &refresh)
{
    // id_token for alice@example.org, unsigned: KRDC only reads it for a label.
    return QStringLiteral(R"({"access_token":"%1","refresh_token":"%2","expires_in":3600,)"
                          R"("id_token":"eyJhbGciOiJub25lIn0.eyJwcmVmZXJyZWRfdXNlcm5hbWUiOiJhbGljZUBleGFtcGxlLm9yZyJ9."})")
        .arg(access, refresh)
        .toUtf8();
}

void routeFeed(FakeNetwork &network)
{
    network.route(DiscoveryUrl, 200, Discovery);
    network.route(FeedUrl, 200, Feed);
    network.route(DesktopRdpUrl, 200, DesktopRdp);
}

bool filesContain(const QString &dir, const QByteArray &needle)
{
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile file(it.next());
        if (file.open(QIODevice::ReadOnly) && file.readAll().contains(needle)) {
            return true;
        }
    }
    return false;
}
}

class AvdSignInDialogTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void requiresConsentToHandleLinks();
    void namesCurrentHandlerBeforeReplacing();
    void opensBrowserOnceOnRequest();
    void showsBrowserLaunchError();
    void completesWithCallbackFromHandler();
    void cancelInvalidatesCallback();
    void wrongDestinationKeepsSignIn();
    void systemBrowserReportsLaunch();
    void signOutDuringSignInDiscardsAnswer();
    void closingWindowDuringSignInIsSafe();
    void refusesAnswerOfOtherSignIn();
    void workspaceSignInUsesBrokerRedirect();
    void signsInListsAndLaunchesOnlyOnRequest();
    void signsInSilentlyWithStoredToken();
    void staleRefreshTokenFallsBackToSignIn();
    void signOutForgetsToken();
    void signOutWinsOverPendingRead();
    void accountChangeIgnoresPendingRead();
    void untrustedGatewayGetsNoToken();
};

void AvdSignInDialogTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QVERIFY(qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS") == QLatin1String("disabled:"));
    // Sockets a crashed earlier run of the test left behind.
    CallbackChannel::DirectoryError dirError;
    const QString channel = CallbackChannel::directory(true, &dirError);
    QVERIFY(!channel.isEmpty());
    for (const QString &name : QDir(channel).entryList(QDir::System | QDir::Files | QDir::NoDotAndDotDot)) {
        QFile::remove(channel + QLatin1Char('/') + name);
    }
}

namespace
{
const QString Redirect = brokerRedirectUri(publicCloud().clientId);
const QString State = QStringLiteral("c3ludGhldGljLXN0YXRlLWZvci1rcmRjLXRlc3Rz");
const QUrl Authorize(QStringLiteral("https://login.microsoftonline.com/organizations/oauth2/v2.0/authorize?state=c3ludGhldGljLXN0YXRlLWZvci1rcmRjLXRlc3Rz"));

AvdSignInDialog::Request dialogRequest()
{
    return {Authorize, Redirect, State, QStringLiteral("test")};
}

int runHandler(const QByteArray &link)
{
    QProcess process;
    process.start(QStringLiteral(KRDC_AVD_HANDLER_PATH), {QString::fromUtf8(link)});
    if (!QTest::qWaitFor(
            [&process]() {
                return process.state() == QProcess::NotRunning;
            },
            10000)) {
        process.kill();
        process.waitForFinished();
        return -1;
    }
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}

QByteArray callbackLink(const QString &state, const QString &redirect = Redirect)
{
    return redirect.toUtf8() + "?code=synthetic-code&state=" + state.toUtf8() + "&session_state=s";
}

// Registers the built helper in the private test configuration.
void registerHandler()
{
    Avd::HandlerRegistration registration(QStringLiteral(KRDC_AVD_HANDLER_PATH));
    QString error;
    QVERIFY2(registration.registerHandler(&error), qPrintable(error));
}

// Emptied rather than removed: KSycoca notices changed files and directories, not missing ones.
void unregisterHandler()
{
    const QString apps = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    QDir(apps).removeRecursively();
    QVERIFY(QDir().mkpath(apps));
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QVERIFY(QDir().mkpath(config));
    QFile mimeapps(config + QStringLiteral("/mimeapps.list"));
    QVERIFY(mimeapps.open(QIODevice::WriteOnly | QIODevice::Truncate));
}

// KSycoca looks at changed files at most every 1.5 seconds per thread, but at once in a new one.
void inFreshThread(const std::function<void()> &function)
{
    std::unique_ptr<QThread> thread(QThread::create([&function]() {
        KSycoca::disableAutoRebuild();
        KSycoca::self()->ensureCacheValid();
        function();
    }));
    thread->start();
    thread->wait();
}

QString linkHandler()
{
    QString id;
    inFreshThread([&id]() {
        if (const KService::Ptr service = KApplicationTrader::preferredService(QString::fromLatin1(HandlerRegistration::MimeType))) {
            id = service->storageId();
        }
    });
    return id;
}

std::optional<AuthorizationCallback> answerWithCode(const AuthorizationRequest &request)
{
    AuthorizationCallback callback;
    callback.code = QStringLiteral("c");
    callback.encodedCode = "c";
    callback.state = request.state;
    return callback;
}
}

void AvdSignInDialogTest::init()
{
    // Also undoes what a crashed earlier run left behind.
    unregisterHandler();
}

void AvdSignInDialogTest::requiresConsentToHandleLinks()
{
    AvdSignInDialog dialog(dialogRequest());
    dialog.setHandlerRegistration(HandlerRegistration(QStringLiteral(KRDC_AVD_HANDLER_PATH)));
    int opened = 0;
    dialog.setUrlOpener([&opened](const QUrl &, QWidget *, const std::function<void(const QString &)> &done) {
        ++opened;
        done(QString());
    });
    QList<HandlerRegistration::State> asked;
    bool agree = false;
    dialog.setSetupConfirmation([&asked, &agree](const HandlerRegistration::Status &status, const QString &) {
        asked << status.state;
        return agree;
    });

    auto *open = dialog.findChild<QPushButton *>(QStringLiteral("openBrowserButton"));
    auto *setup = dialog.findChild<QPushButton *>(QStringLiteral("setupHandlerButton"));
    QVERIFY(open && setup);
    // Without the handler the sign-in could not come back: nothing to open yet, and no paste field.
    QVERIFY(!open->isEnabled());
    QVERIFY(!setup->isHidden());
    QVERIFY(dialog.findChildren<QLineEdit *>().isEmpty());
    QVERIFY(asked.isEmpty());

    setup->click();
    QCOMPARE(asked, QList<HandlerRegistration::State>{HandlerRegistration::State::NotRegistered});
    QCOMPARE(HandlerRegistration(QStringLiteral(KRDC_AVD_HANDLER_PATH)).status().state, HandlerRegistration::State::NotRegistered);
    QVERIFY(!open->isEnabled());

    agree = true;
    setup->click();
    QCOMPARE(HandlerRegistration(QStringLiteral(KRDC_AVD_HANDLER_PATH)).status().state, HandlerRegistration::State::Registered);
    QVERIFY(open->isEnabled());
    QVERIFY(setup->isHidden());
    QCOMPARE(opened, 0);
}

void AvdSignInDialogTest::namesCurrentHandlerBeforeReplacing()
{
    QVERIFY(QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation)));
    QFile other(QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation) + QStringLiteral("/test.OtherBroker.desktop"));
    QVERIFY(other.open(QIODevice::WriteOnly));
    other.write("[Desktop Entry]\nType=Application\nName=Other Broker\nExec=/usr/bin/false %u\nMimeType=x-scheme-handler/ms-appx-web;\n");
    other.close();
    bool found = false;
    inFreshThread([&found]() {
        const KService::Ptr service = KService::serviceByStorageId(QStringLiteral("test.OtherBroker.desktop"));
        found = service;
        if (service) {
            KApplicationTrader::setPreferredService(QString::fromLatin1(HandlerRegistration::MimeType), service);
        }
    });
    QVERIFY(found);

    AvdSignInDialog dialog(dialogRequest());
    dialog.setHandlerRegistration(HandlerRegistration(QStringLiteral(KRDC_AVD_HANDLER_PATH)));
    auto *label = dialog.findChild<QLabel *>(QStringLiteral("handlerLabel"));
    QVERIFY(label);
    QVERIFY2(label->text().contains(QStringLiteral("Other Broker")), qPrintable(label->text()));

    HandlerRegistration::Status seen;
    dialog.setSetupConfirmation([&seen](const HandlerRegistration::Status &status, const QString &) {
        seen = status;
        return false;
    });
    dialog.findChild<QPushButton *>(QStringLiteral("setupHandlerButton"))->click();
    QCOMPARE(seen.state, HandlerRegistration::State::OtherHandler);
    QCOMPARE(seen.currentName, QStringLiteral("Other Broker"));
    // Declining keeps the other handler.
    QCOMPARE(linkHandler(), QStringLiteral("test.OtherBroker.desktop"));
    QVERIFY(!dialog.findChild<QPushButton *>(QStringLiteral("openBrowserButton"))->isEnabled());
}

void AvdSignInDialogTest::opensBrowserOnceOnRequest()
{
    registerHandler();
    AvdSignInDialog dialog(dialogRequest());
    QList<QUrl> opened;
    std::function<void(const QString &)> finish;
    dialog.setUrlOpener([&opened, &finish](const QUrl &url, QWidget *, const std::function<void(const QString &)> &done) {
        opened << url;
        finish = done;
    });
    // Nothing is opened without the user asking.
    QVERIFY(opened.isEmpty());
    auto *open = dialog.findChild<QPushButton *>(QStringLiteral("openBrowserButton"));
    QVERIFY(open && open->isEnabled());
    open->click();
    QCOMPARE(opened, QList<QUrl>{Authorize});
    // Launching is asynchronous; no second launch meanwhile.
    QVERIFY(!open->isEnabled());
    open->click();
    QCOMPARE(opened.size(), 1);
    finish(QString());
    QVERIFY(open->isEnabled());
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("statusLabel"));
    QVERIFY(status && !status->text().isEmpty());
}

void AvdSignInDialogTest::showsBrowserLaunchError()
{
    registerHandler();
    AvdSignInDialog dialog(dialogRequest());
    int opened = 0;
    dialog.setUrlOpener([&opened](const QUrl &, QWidget *, const std::function<void(const QString &)> &done) {
        ++opened;
        done(QStringLiteral("Could not find the program 'synthetic-browser'"));
    });
    auto *open = dialog.findChild<QPushButton *>(QStringLiteral("openBrowserButton"));
    open->click();
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("statusLabel"));
    QVERIFY2(status->text().contains(QStringLiteral("synthetic-browser")), qPrintable(status->text()));
    // No other way of opening is tried behind the user's back; trying again is up to them.
    QCOMPARE(opened, 1);
    QVERIFY(open->isEnabled());
    QVERIFY(dialog.result() != QDialog::Accepted);
}

void AvdSignInDialogTest::completesWithCallbackFromHandler()
{
    registerHandler();
    AvdSignInDialog dialog(dialogRequest());
    dialog.show();
    QCOMPARE(runHandler(callbackLink(State)), 0);
    QTRY_COMPARE(dialog.result(), int(QDialog::Accepted));
    QVERIFY(dialog.callback());
    QCOMPARE(dialog.callback()->code, QStringLiteral("synthetic-code"));
    QCOMPARE(dialog.callback()->state, State);
}

void AvdSignInDialogTest::cancelInvalidatesCallback()
{
    registerHandler();
    {
        AvdSignInDialog dialog(dialogRequest());
        dialog.show();
        dialog.reject();
        QVERIFY(runHandler(callbackLink(State)) != 0);
        QVERIFY(!dialog.callback());
    }
    // Nor does it reach a later sign-in, which has its own state.
    AvdSignInDialog later({Authorize, Redirect, QStringLiteral("bGF0ZXItc3ludGhldGljLXN0YXRlLWZvci10ZXN0cw"), QStringLiteral("test")});
    QVERIFY(runHandler(callbackLink(State)) != 0);
    QTest::qWait(50);
    QVERIFY(!later.callback());
}

void AvdSignInDialogTest::wrongDestinationKeepsSignIn()
{
    registerHandler();
    AvdSignInDialog dialog(dialogRequest());
    dialog.show();
    // Refused, but any program of the user could send it: the sign-in goes on.
    QVERIFY(runHandler(callbackLink(State, QStringLiteral("ms-appx-web://Microsoft.AAD.BrokerPlugin/00000000-0000-0000-0000-000000000000"))) != 0);
    QTest::qWait(50);
    QVERIFY(dialog.result() != QDialog::Accepted);
    QVERIFY(!dialog.callback());
    QVERIFY(dialog.findChild<QPushButton *>(QStringLiteral("openBrowserButton"))->isEnabled());

    QCOMPARE(runHandler(callbackLink(State)), 0);
    QTRY_COMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.callback()->code, QStringLiteral("synthetic-code"));
}

void AvdSignInDialogTest::systemBrowserReportsLaunch()
{
    // A stand-in browser in the private test configuration; no real browser is involved.
    QTemporaryDir dir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/browser-XXXXXX"));
    QVERIFY(dir.isValid());
    const QString marker = dir.filePath(QStringLiteral("started"));
    const QString script = dir.filePath(QStringLiteral("stub-browser"));
    QFile scriptFile(script);
    QVERIFY(scriptFile.open(QIODevice::WriteOnly));
    scriptFile.write("#!/bin/sh\ntouch '" + marker.toUtf8() + "'\n");
    scriptFile.close();
    QVERIFY(scriptFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QString apps = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    QVERIFY(QDir().mkpath(apps));
    // KIO looks up the browser in this thread's KSycoca, which looks at changed files at most every 1.5 seconds.
    const auto refreshServices = []() {
        QTest::qWait(1600);
        KSycoca::self()->ensureCacheValid();
    };
    // A new file each time: KSycoca notices new files, not changed ones.
    const auto writeBrowser = [&apps, &refreshServices](const QString &name, const QString &exec) {
        QFile desktop(apps + QLatin1Char('/') + name);
        QVERIFY(desktop.open(QIODevice::WriteOnly | QIODevice::Truncate));
        desktop.write("[Desktop Entry]\nType=Application\nName=Stub Browser\nExec=" + exec.toUtf8()
                      + " %U\nMimeType=x-scheme-handler/https;x-scheme-handler/http;text/html;\n");
        desktop.close();
        refreshServices();
        const KService::Ptr stub = KService::serviceByStorageId(name);
        QVERIFY(stub);
        KApplicationTrader::setPreferredService(QStringLiteral("x-scheme-handler/https"), stub);
        refreshServices();
    };
    const auto launch = [](const QString &expected, QString *error) {
        // Refuse to launch anything but the stand-in.
        const KService::Ptr handler = KApplicationTrader::preferredService(QStringLiteral("x-scheme-handler/https"));
        if (!handler || handler->storageId() != expected || !handler->exec().contains(QStringLiteral("browser-"))) {
            qWarning() << "not launching, the https handler is not the stand-in:" << (handler ? handler->storageId() : QString());
            return false;
        }
        bool finished = false;
        AvdSignInDialog::systemBrowser()(QUrl(QStringLiteral("https://krdc-test.invalid/authorize?state=synthetic")),
                                         nullptr,
                                         [&finished, error](const QString &message) {
                                             *error = message;
                                             finished = true;
                                         });
        return QTest::qWaitFor(
            [&finished]() {
                return finished;
            },
            10000);
    };

    writeBrowser(QStringLiteral("test.StubBrowser.desktop"), script);
    QString error = QStringLiteral("unset");
    QVERIFY(launch(QStringLiteral("test.StubBrowser.desktop"), &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QTRY_VERIFY(QFile::exists(marker));

    writeBrowser(QStringLiteral("test.MissingBrowser.desktop"), dir.filePath(QStringLiteral("missing-browser")));
    QVERIFY(launch(QStringLiteral("test.MissingBrowser.desktop"), &error));
    QVERIFY2(error.contains(QStringLiteral("missing-browser")), qPrintable(error));
}

void AvdSignInDialogTest::signOutDuringSignInDiscardsAnswer()
{
    FakeNetwork network;
    routeFeed(network);
    network.route(publicCloud().tokenUrl(), 200, tokenJson(QStringLiteral("feed-access"), QStringLiteral("feed-refresh")));
    MemoryTokenStore store;
    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    dialog.setInteractiveSignIn([&dialog](const AuthorizationRequest &request) {
        // The user signed out (or switched accounts) while the browser was open.
        dialog.signOut();
        return answerWithCode(request);
    });
    dialog.signIn();
    QTest::qWait(100);
    QVERIFY(network.requests.isEmpty());
    QVERIFY(store.tokens.isEmpty());
    QVERIFY(dialog.account().isEmpty());
}

void AvdSignInDialogTest::closingWindowDuringSignInIsSafe()
{
    // The main window, and with it the workspace dialog, goes away while the
    // sign-in dialog of the workspace dialog waits for the browser.
    FakeNetwork network;
    MemoryTokenStore store;
    auto *window = new QWidget;
    QPointer<AvdWorkspaceDialog> workspace = new AvdWorkspaceDialog(&network, &store, publicCloud(), window);
    bool prompted = false;
    QTimer::singleShot(0, window, [&prompted, workspace, window]() {
        const auto prompts = workspace->findChildren<AvdSignInDialog *>();
        prompted = prompts.size() == 1 && prompts.first()->isVisible();
        delete window;
    });
    workspace->signIn();
    QVERIFY(prompted);
    QVERIFY(!workspace);
    QVERIFY(network.requests.isEmpty());
    QVERIFY(store.tokens.isEmpty());
}

void AvdSignInDialogTest::refusesAnswerOfOtherSignIn()
{
    FakeNetwork network;
    MemoryTokenStore store;
    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    dialog.setInteractiveSignIn([](const AuthorizationRequest &request) {
        auto callback = answerWithCode(request);
        callback->state = State;
        return callback;
    });
    dialog.signIn();
    QTest::qWait(50);
    QVERIFY(network.requests.isEmpty());
}

void AvdSignInDialogTest::workspaceSignInUsesBrokerRedirect()
{
    FakeNetwork network;
    MemoryTokenStore store;
    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    AuthorizationRequest seen;
    dialog.setInteractiveSignIn([&seen](const AuthorizationRequest &request) -> std::optional<AuthorizationCallback> {
        seen = request;
        return std::nullopt;
    });
    dialog.signIn();
    const QUrlQuery query(seen.url);
    QCOMPARE(query.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded), Redirect);
    QCOMPARE(query.queryItemValue(QStringLiteral("state")), seen.state);
    QCOMPARE(query.queryItemValue(QStringLiteral("code_challenge_method")), QStringLiteral("S256"));
    QVERIFY(network.requests.isEmpty());
}
void AvdSignInDialogTest::signsInListsAndLaunchesOnlyOnRequest()
{
    FakeNetwork network;
    routeFeed(network);
    network.route(publicCloud().tokenUrl(), 200, tokenJson(QStringLiteral("feed-access"), QStringLiteral("feed-refresh")));
    MemoryTokenStore store;

    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    dialog.setInteractiveSignIn(answerWithCode);
    QSignalSpy launches(&dialog, &AvdWorkspaceDialog::launchRequested);
    QSignalSpy accounts(&dialog, &AvdWorkspaceDialog::accountChanged);

    dialog.signIn();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 2);
    QCOMPARE(dialog.account(), QStringLiteral("alice@example.org"));
    QCOMPARE(accounts.count(), 1);
    QCOMPARE(store.tokens.value(QStringLiteral("alice@example.org")), QStringLiteral("feed-refresh"));
    QVERIFY(!filesContain(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation), "feed-refresh"));

    const QModelIndex desktop = dialog.resourceModel()->index(0, 0);
    const QModelIndex app = dialog.resourceModel()->index(1, 0);
    QVERIFY(desktop.data(AvdResourceModel::LaunchableRole).toBool());
    QVERIFY(!app.data(AvdResourceModel::LaunchableRole).toBool());

    // Listing does not open anything.
    QTest::qWait(50);
    QCOMPARE(launches.count(), 0);
    const int requestsBeforeConnect = network.requests.size();

    dialog.resourceView()->setCurrentIndex(dialog.resourceView()->model()->index(0, 0));
    dialog.connectSelected();
    QTRY_COMPARE(launches.count(), 1);
    QCOMPARE(network.requests.size(), requestsBeforeConnect + 1);
    QCOMPARE(network.requests.last().request.url(), DesktopRdpUrl);

    const auto launch = launches.first().at(0).value<ConnectionLaunch>();
    QVERIFY(launch.isValid());
    QVERIFY(!launch.remember);
    QCOMPARE(launch.name, QStringLiteral("Example Desktop"));
    QVERIFY(!launch.url.toString().contains(QStringLiteral("feed-access")));
    const auto *profile = dynamic_cast<const RdpLaunchProfile *>(launch.payload.get());
    QVERIFY(profile);
    QVERIFY(profile->isAzureVirtualDesktop());
    QVERIFY(profile->hasGatewayBearerToken());
    QCOMPARE(profile->source(), RdpLaunchProfile::Source::AvdWorkspace);
    // Not an untrusted file: nothing goes through RemoteViewFactory::importFileSettings().
    QVERIFY(!launch.fromFile);
    QVERIFY(launch.fileSettings.isEmpty());
}

void AvdSignInDialogTest::signsInSilentlyWithStoredToken()
{
    FakeNetwork network;
    routeFeed(network);
    network.route(publicCloud().tokenUrl(), 200, tokenJson(QStringLiteral("a2"), QStringLiteral("rotated")));
    MemoryTokenStore store;
    store.tokens.insert(QStringLiteral("alice@example.org"), QStringLiteral("stored"));

    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    bool prompted = false;
    dialog.setInteractiveSignIn([&prompted](const AuthorizationRequest &) -> std::optional<AuthorizationCallback> {
        prompted = true;
        return std::nullopt;
    });
    dialog.setAccount(QStringLiteral("alice@example.org"));
    dialog.start();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 2);
    QVERIFY(!prompted);
    QCOMPARE(store.tokens.value(QStringLiteral("alice@example.org")), QStringLiteral("rotated"));
    const QUrlQuery form(QString::fromUtf8(network.requests.first().body));
    QCOMPARE(form.queryItemValue(QStringLiteral("refresh_token")), QStringLiteral("stored"));
}

void AvdSignInDialogTest::staleRefreshTokenFallsBackToSignIn()
{
    FakeNetwork network;
    network.route(publicCloud().tokenUrl(), 400, R"({"error":"invalid_grant","error_description":"AADSTS70008: expired"})");
    MemoryTokenStore store;
    store.tokens.insert(QStringLiteral("alice@example.org"), QStringLiteral("stale"));

    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    bool prompted = false;
    dialog.setInteractiveSignIn([&prompted](const AuthorizationRequest &) -> std::optional<AuthorizationCallback> {
        prompted = true;
        return std::nullopt;
    });
    dialog.setAccount(QStringLiteral("alice@example.org"));
    dialog.start();
    QTRY_VERIFY(!store.tokens.contains(QStringLiteral("alice@example.org")));
    // Silent sign-in failing must not pop up a browser on its own.
    QVERIFY(!prompted);
    QCOMPARE(dialog.resourceModel()->rowCount(), 0);
}

void AvdSignInDialogTest::signOutForgetsToken()
{
    FakeNetwork network;
    MemoryTokenStore store;
    store.tokens.insert(QStringLiteral("alice@example.org"), QStringLiteral("stored"));
    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    dialog.setAccount(QStringLiteral("alice@example.org"));
    dialog.signOut();
    QVERIFY(store.tokens.isEmpty());
    QVERIFY(dialog.account().isEmpty());
}

void AvdSignInDialogTest::signOutWinsOverPendingRead()
{
    FakeNetwork network;
    routeFeed(network);
    network.route(publicCloud().tokenUrl(), 200, tokenJson(QStringLiteral("a2"), QStringLiteral("rotated")));
    DeferredTokenStore store;

    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    QSignalSpy accounts(&dialog, &AvdWorkspaceDialog::accountChanged);
    dialog.setAccount(QStringLiteral("alice@example.org"));
    dialog.start();
    QCOMPARE(store.pending.size(), 1);

    dialog.signOut();
    store.completeReads();
    QTest::qWait(100);

    QVERIFY(network.requests.isEmpty());
    QVERIFY(dialog.account().isEmpty());
    QVERIFY(store.tokens.isEmpty());
    QCOMPARE(accounts.count(), 1);
    QVERIFY(accounts.last().at(0).toString().isEmpty());
    QCOMPARE(dialog.resourceModel()->rowCount(), 0);
}

void AvdSignInDialogTest::accountChangeIgnoresPendingRead()
{
    FakeNetwork network;
    routeFeed(network);
    network.route(publicCloud().tokenUrl(), 200, tokenJson(QStringLiteral("a2"), QStringLiteral("rotated")));
    DeferredTokenStore store;

    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    dialog.setAccount(QStringLiteral("alice@example.org"));
    dialog.start();
    dialog.setAccount(QStringLiteral("bob@example.org"));
    store.completeReads();
    QTest::qWait(100);

    // Alice's stored sign-in is not used for Bob.
    QVERIFY(network.requests.isEmpty());
    QCOMPARE(dialog.account(), QStringLiteral("bob@example.org"));
    QVERIFY(store.tokens.isEmpty());
}

void AvdSignInDialogTest::untrustedGatewayGetsNoToken()
{
    FakeNetwork network;
    routeFeed(network);
    network.route(DesktopRdpUrl, 200, "full address:s:hostpool.example.org\r\ngatewayhostname:s:gateway.attacker.example:443\r\nresourceprovider:s:arm\r\n");
    network.route(publicCloud().tokenUrl(), 200, tokenJson(QStringLiteral("feed-access"), QStringLiteral("feed-refresh")));
    MemoryTokenStore store;

    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    dialog.setInteractiveSignIn(answerWithCode);
    QSignalSpy launches(&dialog, &AvdWorkspaceDialog::launchRequested);
    dialog.signIn();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 2);

    const int requestsBeforeConnect = network.requests.size();
    dialog.resourceView()->setCurrentIndex(dialog.resourceView()->model()->index(0, 0));
    dialog.connectSelected();
    QTRY_COMPARE(network.requests.size(), requestsBeforeConnect + 1);
    QTest::qWait(100);
    QCOMPARE(launches.count(), 0);
    QVERIFY(dialog.result() != QDialog::Accepted);
}

QTEST_MAIN(AvdSignInDialogTest)

#include "avdsignindialogtest.moc"
