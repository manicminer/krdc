/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "remoteviewfactory.h"

#include <KConfig>
#include <KConfigGroup>
#include <KPluginFactory>
#include <KPluginMetaData>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
// Accepts one client on 127.0.0.1 and selects standard RDP security, after
// which FreeRDP asks for credentials. Nothing leaves this machine.
class FakeRdpServer
{
public:
    FakeRdpServer()
    {
        m_listen = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(address);
        if (m_listen < 0 || bind(m_listen, reinterpret_cast<sockaddr *>(&address), length) != 0 || listen(m_listen, 1) != 0
            || getsockname(m_listen, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
            return;
        }
        m_port = ntohs(address.sin_port);
        m_thread = std::thread([this]() {
            m_client = accept(m_listen, nullptr, nullptr);
            if (m_client < 0) {
                return;
            }
            char request[512];
            if (read(m_client, request, sizeof(request)) <= 0) {
                return;
            }
            // TPKT, X.224 connection confirm, RDP_NEG_RSP with PROTOCOL_RDP.
            static const unsigned char confirm[] =
                {0x03, 0x00, 0x00, 0x13, 0x0e, 0xd0, 0x00, 0x00, 0x12, 0x34, 0x00, 0x02, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00};
            if (write(m_client, confirm, sizeof(confirm)) == sizeof(confirm)) {
                m_answered = true;
            }
        });
    }
    ~FakeRdpServer()
    {
        shutdown(m_listen, SHUT_RDWR);
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_client >= 0) {
            close(m_client);
        }
        if (m_listen >= 0) {
            close(m_listen);
        }
    }
    int port() const
    {
        return m_port;
    }
    bool answered() const
    {
        return m_answered;
    }

private:
    int m_listen = -1;
    int m_client = -1;
    int m_port = 0;
    std::atomic_bool m_answered = false;
    std::thread m_thread;
};

// Fails the test instead of hanging it if the GUI thread deadlocks.
class Watchdog
{
public:
    explicit Watchdog(const char *what)
        : m_thread([this, what]() {
            std::unique_lock lock(m_mutex);
            if (!m_done.wait_for(lock, std::chrono::seconds(30), [this]() {
                    return m_released;
                })) {
                qFatal("Deadlock: %s did not return", what);
            }
        })
    {
    }
    ~Watchdog()
    {
        {
            std::lock_guard lock(m_mutex);
            m_released = true;
        }
        m_done.notify_all();
        m_thread.join();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_done;
    bool m_released = false;
    std::thread m_thread;
};
}

// Loads the built RDP plugin and drives it as MainWindow does.
class RdpPluginTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void importsFileWithoutCredentialsInUrl();
    void connectionFileKeepsConventionalSettings();
    void importsConventionalSettingsIntoNewHost();
    void asksBeforeFileGateway_data();
    void asksBeforeFileGateway();
    void unusedFileGatewayIsNotImported();
    void refusesRemoteAppAndWorkspaceFiles();
    void importedLaunchStartsWithoutLockingUser();
    void offersWorkspaceAction();
    void opensOneWorkspaceWindowOnly();
    void closingWithQueuedCredentialRequest();
    void closingDuringCredentialPrompt();

private:
    QString writeFile(const QString &name, const QByteArray &contents);
    RemoteView *createPromptingView(QWidget *window, KConfig &config, int port);
    int importCancellingPrompts(const ConnectionLaunch &launch, KConfigGroup group, bool *imported);

    QTemporaryDir m_dir;
    RemoteViewFactory *m_factory = nullptr;
};

void RdpPluginTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    const KPluginMetaData metaData(QStringLiteral(KRDC_RDP_PLUGIN_PATH));
    QVERIFY2(metaData.isValid(), KRDC_RDP_PLUGIN_PATH);
    auto result = KPluginFactory::instantiatePlugin<RemoteViewFactory>(metaData, this);
    QVERIFY2(result, qPrintable(result.errorString));
    m_factory = result.plugin;
}

QString RdpPluginTest::writeFile(const QString &name, const QByteArray &contents)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(contents);
    }
    return path;
}

void RdpPluginTest::importsFileWithoutCredentialsInUrl()
{
    const QString path =
        writeFile(QStringLiteral("desktop.rdp"), "full address:s:[2001:db8::5]:3390\r\nusername:s:alice@example.org\r\npassword:s:not-in-url\r\n");
    QString error;
    const ConnectionLaunch launch = m_factory->loadLaunchFromFile(QUrl::fromLocalFile(path), &error);
    QVERIFY2(launch.isValid(), qPrintable(error));
    QVERIFY(!launch.remember);
    QCOMPARE(launch.url.host(), QStringLiteral("2001:db8::5"));
    QCOMPARE(launch.url.port(), 3390);
    QVERIFY(launch.url.userInfo().isEmpty());
    QVERIFY(launch.payload);
}

void RdpPluginTest::connectionFileKeepsConventionalSettings()
{
    const QString path = writeFile(QStringLiteral("legacy.rdp"),
                                   "full address:s:[2001:db8::5]\r\nserver port:i:3390\r\nusername:s:alice\r\npassword:s:se:cret\r\n"
                                   "domain:s:CORP\r\ndesktopwidth:i:1280\r\n");
    const RemoteViewFactory::ConnectionFile file = m_factory->loadConnectionFile(QUrl::fromLocalFile(path));
    QCOMPARE(file.url.host(), QStringLiteral("2001:db8::5"));
    QCOMPARE(file.url.port(), 3390);
    QCOMPARE(file.url.userName(), QStringLiteral("alice"));
    QCOMPARE(file.url.query(), QStringLiteral("domain=CORP"));
    QVERIFY(file.url.password().isEmpty());
    QVERIFY(!file.settings.contains(QStringLiteral("password")));
    QCOMPARE(file.settings.value(QStringLiteral("desktopwidth")), QStringLiteral("1280"));
}

void RdpPluginTest::importsConventionalSettingsIntoNewHost()
{
    const QString path = writeFile(QStringLiteral("settings.rdp"),
                                   "full address:s:desktop.example.org\r\npassword:s:secret\r\ndesktopwidth:i:1280\r\ndesktopheight:i:720\r\n"
                                   "smart sizing:i:1\r\naudiomode:i:1\r\nredirectclipboard:i:0\r\ndrivestoredirect:s:*\r\n");
    QString error;
    const ConnectionLaunch launch = m_factory->loadLaunchFromFile(QUrl::fromLocalFile(path), &error);
    QVERIFY2(launch.isValid(), qPrintable(error));
    // Still the typed launch FreeRDP applies, now also carrying the settings for the host.
    QVERIFY(launch.payload);
    QVERIFY(!launch.remember);
    QVERIFY(launch.fromFile);
    QVERIFY(!launch.fileSettings.contains(QStringLiteral("password")));

    KConfig config(m_dir.filePath(QStringLiteral("importrc")), KConfig::SimpleConfig);
    KConfigGroup group = config.group(QStringLiteral("hostpreferences")).group(launch.url.toDisplayString());
    QVERIFY(!group.exists());
    QVERIFY(m_factory->importFileSettings(launch.fileSettings, group, nullptr));
    QCOMPARE(group.readEntry("resolution", -1), 5); // Custom
    QCOMPARE(group.readEntry("width", 0), 1280);
    QCOMPARE(group.readEntry("height", 0), 720);
    QCOMPARE(group.readEntry("windowedScale", false), true);
    QCOMPARE(group.readEntry("sound", -1), 1); // Remote
    QCOMPARE(group.readEntry("clipboardSharing", true), false);
    QVERIFY(!group.hasKey("useGateway"));
    for (const QString &key : group.keyList()) {
        QVERIFY2(!key.contains(QStringLiteral("drive"), Qt::CaseInsensitive), qPrintable(key));
    }

    // A host with settings keeps them.
    group.writeEntry("sound", 2);
    QHash<QString, QString> other = launch.fileSettings;
    other.insert(QStringLiteral("audiomode"), QStringLiteral("0"));
    QVERIFY(m_factory->importFileSettings(other, group, nullptr));
    QCOMPARE(group.readEntry("sound", -1), 2);
}

// Runs importFileSettings(), cancelling each dialog it shows. Returns how many it showed.
int RdpPluginTest::importCancellingPrompts(const ConnectionLaunch &launch, KConfigGroup group, bool *imported)
{
    QWidget window;
    int prompts = 0;
    QPointer<QDialog> last;
    QTimer cancel;
    connect(&cancel, &QTimer::timeout, this, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog && dialog != last) {
            last = dialog;
            ++prompts;
            dialog->reject();
        }
    });
    cancel.start(20);
    *imported = m_factory->importFileSettings(launch.fileSettings, group, &window);
    return prompts;
}

void RdpPluginTest::asksBeforeFileGateway_data()
{
    QTest::addColumn<QByteArray>("method");
    QTest::newRow("always") << QByteArray("1");
    QTest::newRow("bypass local") << QByteArray("2");
    // FreeRDP reads these as 1, so they must not slip past the question.
    QTest::newRow("trailing text") << QByteArray("1x");
    QTest::newRow("leading space") << QByteArray(" 1");
    QTest::newRow("hex") << QByteArray("0x1");
}

void RdpPluginTest::asksBeforeFileGateway()
{
    QFETCH(QByteArray, method);
    const QString path = writeFile(QStringLiteral("gateway.rdp"),
                                   "full address:s:desktop.example.org\r\ngatewayhostname:s:gw.example.org\r\ngatewayusagemethod:i:" + method + "\r\n");
    QString error;
    const ConnectionLaunch launch = m_factory->loadLaunchFromFile(QUrl::fromLocalFile(path), &error);
    QVERIFY2(launch.isValid(), qPrintable(error));

    // The launch connects through the file's gateway even for a known host, so it asks there too.
    KConfig config(m_dir.filePath(QStringLiteral("gatewayrc")), KConfig::SimpleConfig);
    KConfigGroup group = config.group(QStringLiteral("hostpreferences")).group(launch.url.toDisplayString());
    group.writeEntry("sound", 0);
    bool imported = true;
    QCOMPARE(importCancellingPrompts(launch, group, &imported), 1);
    QVERIFY(!imported);
    QVERIFY(!group.hasKey("useGateway"));
}

void RdpPluginTest::unusedFileGatewayIsNotImported()
{
    const QString path =
        writeFile(QStringLiteral("nogateway.rdp"), "full address:s:desktop.example.org\r\ngatewayhostname:s:gw.example.org\r\ngatewayusagemethod:i:0\r\n");
    QString error;
    const ConnectionLaunch launch = m_factory->loadLaunchFromFile(QUrl::fromLocalFile(path), &error);
    QVERIFY2(launch.isValid(), qPrintable(error));
    KConfig config(m_dir.filePath(QStringLiteral("nogatewayrc")), KConfig::SimpleConfig);
    KConfigGroup group = config.group(QStringLiteral("hostpreferences")).group(launch.url.toDisplayString());
    bool imported = false;
    QCOMPARE(importCancellingPrompts(launch, group, &imported), 0);
    QVERIFY(imported);
    QVERIFY(!group.hasKey("useGateway"));
}

void RdpPluginTest::refusesRemoteAppAndWorkspaceFiles()
{
    QString error;
    const QString app = writeFile(QStringLiteral("app.rdp"), "full address:s:h.example.org\r\nremoteapplicationmode:i:1\r\n");
    QVERIFY(!m_factory->loadLaunchFromFile(QUrl::fromLocalFile(app), &error).isValid());
    QVERIFY(!error.isEmpty());

    error.clear();
    const QString workspace = writeFile(QStringLiteral("ws.rdpw"), "anything");
    QVERIFY(!m_factory->loadLaunchFromFile(QUrl::fromLocalFile(workspace), &error).isValid());
    QVERIFY(error.contains(QStringLiteral(".rdpw")));

    error.clear();
    QVERIFY(!m_factory->loadLaunchFromFile(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("x.vnc"))), &error).isValid());
    QVERIFY(error.isEmpty());
}

void RdpPluginTest::importedLaunchStartsWithoutLockingUser()
{
    // A closed local port: FreeRDP fails at once, without leaving this machine.
    const QString path = writeFile(QStringLiteral("local.rdp"), "full address:s:127.0.0.1:1\r\nusername:s:imported@example.org\r\n");
    QString error;
    const ConnectionLaunch launch = m_factory->loadLaunchFromFile(QUrl::fromLocalFile(path), &error);
    QVERIFY2(launch.isValid(), qPrintable(error));

    QWidget window;
    window.resize(800, 600);
    KConfig config(m_dir.filePath(QStringLiteral("krdcrc")), KConfig::SimpleConfig);
    RemoteView *view = m_factory->createViewForLaunch(&window, launch, config.group(QStringLiteral("hostpreferences")).group(launch.url.toDisplayString()));
    QVERIFY(view);
    QCOMPARE(view->url(), launch.url);

    QSignalSpy errors(view, &RemoteView::connectionError);
    QSignalSpy disconnected(view, &RemoteView::disconnected);
    view->start();
    QTRY_VERIFY_WITH_TIMEOUT(errors.count() > 0 || disconnected.count() > 0, 20000);
    // Nothing was added to the address: the imported user stays a suggestion.
    QVERIFY(view->url().userInfo().isEmpty());
    view->startQuitting();
    delete view;
}

void RdpPluginTest::offersWorkspaceAction()
{
    QWidget window;
    const QList<QAction *> actions = m_factory->createActions(&window);
    QCOMPARE(actions.size(), 1);
    QCOMPARE(actions.first()->objectName(), QStringLiteral("rdp_avd_workspaces"));
}

void RdpPluginTest::opensOneWorkspaceWindowOnly()
{
    // No account is configured in the isolated profile, so the window neither signs in nor reads the keychain.
    QWidget window;
    const QList<QAction *> actions = m_factory->createActions(&window);
    QCOMPARE(actions.size(), 1);
    const auto workspaceWindows = [&window]() {
        QList<QDialog *> found;
        for (QDialog *dialog : window.findChildren<QDialog *>()) {
            if (dialog->inherits("AvdWorkspaceDialog")) {
                found << dialog;
            }
        }
        return found;
    };
    actions.first()->trigger();
    QCOMPARE(workspaceWindows().size(), 1);
    const QPointer<QDialog> first = workspaceWindows().first();
    QVERIFY(first->isVisible());

    // The windows share one cache: a second could put back the list a sign-out in the first removed.
    actions.first()->trigger();
    QCOMPARE(workspaceWindows().size(), 1);
    QCOMPARE(workspaceWindows().first(), first.data());

    // Once closed, it may be opened again.
    first->close();
    QTRY_VERIFY(!first);
    actions.first()->trigger();
    QCOMPARE(workspaceWindows().size(), 1);
    workspaceWindows().first()->close();
}

RemoteView *RdpPluginTest::createPromptingView(QWidget *window, KConfig &config, int port)
{
    const QString path = writeFile(QStringLiteral("prompt.rdp"), "full address:s:127.0.0.1:" + QByteArray::number(port) + "\r\n");
    QString error;
    const ConnectionLaunch launch = m_factory->loadLaunchFromFile(QUrl::fromLocalFile(path), &error);
    if (!launch.isValid()) {
        return nullptr;
    }
    return m_factory->createViewForLaunch(window, launch, config.group(QStringLiteral("hostpreferences")).group(launch.url.toDisplayString()));
}

void RdpPluginTest::closingWithQueuedCredentialRequest()
{
    FakeRdpServer server;
    QVERIFY(server.port() > 0);
    QWidget window;
    KConfig config(m_dir.filePath(QStringLiteral("queuedrc")), KConfig::SimpleConfig);
    RemoteView *view = createPromptingView(&window, config, server.port());
    QVERIFY(view);

    view->start();
    // The request for credentials is posted to this thread, which does not get to it before closing.
    for (int i = 0; i < 100 && !server.answered(); ++i) {
        QThread::msleep(50);
    }
    QVERIFY(server.answered());
    QThread::msleep(500);
    {
        const Watchdog watchdog("closing with a queued credential request");
        view->startQuitting();
    }
    // The canceled request opens no dialog afterwards.
    QTest::qWait(200);
    QVERIFY(!QApplication::activeModalWidget());
    delete view;
}

void RdpPluginTest::closingDuringCredentialPrompt()
{
    FakeRdpServer server;
    QVERIFY(server.port() > 0);
    QWidget window;
    KConfig config(m_dir.filePath(QStringLiteral("promptrc")), KConfig::SimpleConfig);
    RemoteView *view = createPromptingView(&window, config, server.port());
    QVERIFY(view);

    bool closed = false;
    QPointer<QWidget> prompt;
    QTimer poll;
    poll.setInterval(20);
    connect(&poll, &QTimer::timeout, this, [&]() {
        if (closed || !QApplication::activeModalWidget()) {
            return;
        }
        // The window is closed while the prompt waits for the user.
        prompt = QApplication::activeModalWidget();
        closed = true;
        {
            const Watchdog watchdog("closing during a credential prompt");
            view->startQuitting();
        }
        if (prompt) {
            // Not left behind for a connection that is gone.
            QVERIFY(!prompt->isVisible());
            prompt->close();
        }
    });
    poll.start();

    view->start();
    QTRY_VERIFY_WITH_TIMEOUT(closed, 20000);
    QTRY_VERIFY(!QApplication::activeModalWidget());
    delete view;
}

QTEST_MAIN(RdpPluginTest)

#include "rdpplugintest.moc"
