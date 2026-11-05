/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdcallback.h"
#include "avd/avdcallbackchannel.h"

#include <QDir>
#include <QFile>
#include <QLocalSocket>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <vector>

#include <sys/stat.h>
#include <sys/un.h>

using namespace Avd;

namespace
{
const QString ClientId = QStringLiteral("a85cf173-4192-42f8-81fa-777a763e6e2c");
const QString Redirect = QStringLiteral("ms-appx-web://Microsoft.AAD.BrokerPlugin/a85cf173-4192-42f8-81fa-777a763e6e2c");
const QString State = QStringLiteral("c3ludGhldGljLXN0YXRlLWZvci1rcmRjLXRlc3Rz");
const QString OtherState = QStringLiteral("b3RoZXItc3ludGhldGljLXN0YXRlLWZvci10ZXN0");
const QByteArray Code = "synthetic.code-value_1";

QByteArray link(const QByteArray &query)
{
    return Redirect.toUtf8() + '?' + query;
}

QByteArray goodLink(const QString &state = State)
{
    return link("code=" + Code + "&state=" + state.toUtf8() + "&session_state=synthetic-session");
}

struct HandlerRun {
    int exitCode = -1;
    QByteArray output;
};

HandlerRun runHandler(const QByteArray &argument)
{
    QProcess process;
    process.setProgram(QStringLiteral(KRDC_AVD_HANDLER_PATH));
    process.setArguments({QString::fromUtf8(argument)});
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    // The receiver lives on this thread, so the event loop has to run meanwhile.
    if (!QTest::qWaitFor(
            [&process]() {
                return process.state() == QProcess::NotRunning;
            },
            10000)) {
        process.kill();
        process.waitForFinished();
    }
    return {process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1, process.readAll()};
}

class EnvironmentOverride
{
public:
    EnvironmentOverride(const char *name, const QByteArray &value)
        : m_name(name)
        , m_had(qEnvironmentVariableIsSet(name))
        , m_saved(qgetenv(name))
    {
        if (value.isNull()) {
            qunsetenv(name);
        } else {
            qputenv(name, value);
        }
    }
    ~EnvironmentOverride()
    {
        if (m_had) {
            qputenv(m_name, m_saved);
        } else {
            qunsetenv(m_name);
        }
    }

private:
    const char *m_name;
    bool m_had;
    QByteArray m_saved;
};

QString channelPath(const QString &state)
{
    CallbackChannel::DirectoryError dirError;
    return CallbackChannel::directory(false, &dirError) + QLatin1Char('/') + CallbackChannel::socketName(state);
}

QFile::Permissions ownerOnly()
{
    return QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
}
}

class AvdCallbackTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void brokerRedirect();
    void acceptsCode();
    void acceptsAuthorityError();
    void acceptsLinksAsDesktopsPassThem();
    void refusesMalformed_data();
    void refusesMalformed();
    void stateForRouting();
    void directoryIsOwnerOnly();
    void refusesInsecureDirectories();
    void handlerDeliversToLiveSignIn();
    void handlerDoesNotReachOtherSignIns();
    void malformedLinkKeepsSignIn();
    void replayReachesNothing();
    void cancelInvalidatesCallbacks();
    void cancelDropsPendingClients();
    void timesOut();
    void boundsMessageSize();
    void stalledClientDoesNotBlockSignIn();
    void boundsConcurrentClients();
    void socketIsOwnerOnly();
    void handlerRefusesForeignLinks();
};

void AvdCallbackTest::initTestCase()
{
    // Never touch the runtime directory of a real session.
    QVERIFY(qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS") == QLatin1String("disabled:"));
    QVERIFY(QFileInfo(QStringLiteral(KRDC_AVD_HANDLER_PATH)).isExecutable());
    // Sockets a crashed earlier run of the test left behind.
    CallbackChannel::DirectoryError dirError;
    const QString channel = CallbackChannel::directory(true, &dirError);
    QVERIFY(!channel.isEmpty());
    for (const QString &name : QDir(channel).entryList(QDir::System | QDir::Files | QDir::NoDotAndDotDot)) {
        QFile::remove(channel + QLatin1Char('/') + name);
    }
}

void AvdCallbackTest::brokerRedirect()
{
    QCOMPARE(brokerRedirectUri(ClientId), Redirect);
}

void AvdCallbackTest::acceptsCode()
{
    QString error;
    const auto callback = parseAuthorizationCallback(link("code=a%2Fb%3D&state=" + State.toUtf8() + "&session_state=s"), Redirect, State, &error);
    QVERIFY2(callback, qPrintable(error));
    QCOMPARE(callback->code, QStringLiteral("a/b="));
    QCOMPARE(callback->encodedCode, QByteArray("a%2Fb%3D"));
    QCOMPARE(callback->state, State);
    QVERIFY(!callback->isError());

    // Host names are not case sensitive; browsers may hand them over in lower case.
    QVERIFY(parseAuthorizationCallback("ms-appx-web://microsoft.aad.brokerplugin/" + ClientId.toUtf8() + "?code=c&state=" + State.toUtf8(),
                                       Redirect,
                                       State,
                                       &error));
    // So is the scheme.
    QVERIFY2(parseAuthorizationCallback("MS-APPX-WEB://Microsoft.AAD.BrokerPlugin/" + ClientId.toUtf8() + "?code=c&state=" + State.toUtf8(),
                                        Redirect,
                                        State,
                                        &error),
             qPrintable(error));
}

void AvdCallbackTest::acceptsAuthorityError()
{
    QString error;
    const auto callback =
        parseAuthorizationCallback(link("error=access_denied&error_description=AADSTS53003%3A+Blocked+by+Conditional+Access&state=" + State.toUtf8()),
                                   Redirect,
                                   State,
                                   &error);
    QVERIFY2(callback, qPrintable(error));
    QVERIFY(callback->isError());
    QVERIFY(callback->code.isEmpty());
    QCOMPARE(callback->error, QStringLiteral("access_denied"));
    QCOMPARE(callback->errorDescription, QStringLiteral("AADSTS53003: Blocked by Conditional Access"));
}

void AvdCallbackTest::acceptsLinksAsDesktopsPassThem()
{
    // As xdg-open hands them over on Plasma (through kde-open and KIO): host in
    // lower case, %20 and %7E decoded. Error descriptions carry CR LF lines.
    QString error;
    const auto refused =
        parseAuthorizationCallback("ms-appx-web://microsoft.aad.brokerplugin/" + ClientId.toUtf8()
                                       + "?error=access_denied&error_description=AADSTS53003%3A+Blocked by CA%0D%0ATrace ID: x&state=" + State.toUtf8(),
                                   Redirect,
                                   State,
                                   &error);
    QVERIFY2(refused, qPrintable(error));
    QCOMPARE(refused->errorDescription, QStringLiteral("AADSTS53003: Blocked by CA Trace ID: x"));

    const auto code = parseAuthorizationCallback("ms-appx-web://microsoft.aad.brokerplugin/" + ClientId.toUtf8()
                                                     + "?code=sy%2Fn.t_h-e~t%2Bic%3D&state=" + State.toUtf8() + "&session_state=s~x",
                                                 Redirect,
                                                 State,
                                                 &error);
    QVERIFY2(code, qPrintable(error));
    QCOMPARE(code->code, QStringLiteral("sy/n.t_h-e~t+ic="));
    QCOMPARE(code->encodedCode, QByteArray("sy%2Fn.t_h-e~t%2Bic%3D"));
}

void AvdCallbackTest::refusesMalformed_data()
{
    QTest::addColumn<QByteArray>("link");
    const QByteArray state = State.toUtf8();
    const QByteArray id = ClientId.toUtf8();
    QTest::newRow("nativeclient") << QByteArray("https://login.microsoftonline.com/common/oauth2/nativeclient?code=" + Code + "&state=" + state);
    QTest::newRow("other scheme") << QByteArray("ms-appx-webx://Microsoft.AAD.BrokerPlugin/" + id + "?code=" + Code + "&state=" + state);
    QTest::newRow("other host") << QByteArray("ms-appx-web://Microsoft.AAD.BrokerPlugin.example/" + id + "?code=" + Code + "&state=" + state);
    QTest::newRow("other client") << QByteArray("ms-appx-web://Microsoft.AAD.BrokerPlugin/00000000-0000-0000-0000-000000000000?code=" + Code
                                                + "&state=" + state);
    QTest::newRow("longer path") << QByteArray("ms-appx-web://Microsoft.AAD.BrokerPlugin/" + id + "/x?code=" + Code + "&state=" + state);
    QTest::newRow("user info") << QByteArray("ms-appx-web://u@Microsoft.AAD.BrokerPlugin/" + id + "?code=" + Code + "&state=" + state);
    QTest::newRow("port") << QByteArray("ms-appx-web://Microsoft.AAD.BrokerPlugin:1/" + id + "?code=" + Code + "&state=" + state);
    QTest::newRow("fragment") << QByteArray(link("code=" + Code + "&state=" + state) + "#code=other");
    QTest::newRow("no query") << QByteArray(Redirect.toUtf8());
    QTest::newRow("state mismatch") << link("code=" + Code + "&state=" + OtherState.toUtf8());
    QTest::newRow("state prefix") << link("code=" + Code + "&state=" + state + "x");
    QTest::newRow("no state") << link("code=" + Code);
    QTest::newRow("two states") << link("code=" + Code + "&state=" + state + "&state=" + state);
    QTest::newRow("two codes") << link("code=" + Code + "&code=other&state=" + state);
    QTest::newRow("encoded duplicate") << link("code=" + Code + "&%63ode=other&state=" + state);
    QTest::newRow("empty code") << link("code=&state=" + state);
    QTest::newRow("neither") << link("state=" + state);
    QTest::newRow("code and error") << link("code=" + Code + "&error=access_denied&state=" + state);
    QTest::newRow("space") << link("code=a b&state=" + state);
    QTest::newRow("encoded space") << link("code=a%20b&state=" + state);
    QTest::newRow("tab") << link("code=a\tb&state=" + state);
    QTest::newRow("newline") << link("code=a\nb&state=" + state);
    QTest::newRow("encoded control") << link("code=a%00b&state=" + state);
    QTest::newRow("bad escape") << link("code=a%zzb&state=" + state);
    QTest::newRow("non-ascii") << link("code=a\xc3\xa9&state=" + state);
    QTest::newRow("client id case") << QByteArray("ms-appx-web://Microsoft.AAD.BrokerPlugin/" + id.toUpper() + "?code=" + Code + "&state=" + state);
    QTest::newRow("path dot segment") << QByteArray("ms-appx-web://Microsoft.AAD.BrokerPlugin/./" + id + "?code=" + Code + "&state=" + state);
    QTest::newRow("bad escape in other value") << link("code=" + Code + "&state=" + state + "&session_state=%zz");
    QTest::newRow("truncated escape in other value") << link("code=" + Code + "&state=" + state + "&session_state=a%2");
    QTest::newRow("bad escape in description") << link("error=access_denied&error_description=a%g1&state=" + state);
    QTest::newRow("oversized") << link("code=" + QByteArray(CallbackChannel::MaxMessageSize, 'a') + "&state=" + state);
}

void AvdCallbackTest::refusesMalformed()
{
    QFETCH(QByteArray, link);
    QString error;
    QVERIFY(!parseAuthorizationCallback(link, Redirect, State, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!error.contains(State));
    QVERIFY(!error.contains(QString::fromLatin1(Code)));
}

void AvdCallbackTest::stateForRouting()
{
    QCOMPARE(CallbackChannel::stateOf(goodLink()), State);
    QCOMPARE(CallbackChannel::stateOf("MS-APPX-WEB://microsoft.aad.brokerplugin/" + ClientId.toUtf8() + "?code=c&state=" + State.toUtf8()), State);
    QCOMPARE(CallbackChannel::stateOf(link("code=c&state=" + State.toUtf8() + "&state=" + OtherState.toUtf8())), QString());
    QCOMPARE(CallbackChannel::stateOf(link("code=c&state=..%2F..%2Fx")), QString());
    QCOMPARE(CallbackChannel::stateOf(link("code=c&state=short")), QString());
    QCOMPARE(CallbackChannel::stateOf("https://login.microsoftonline.com/common/oauth2/nativeclient?code=c&state=" + State.toUtf8()), QString());

    const QString name = CallbackChannel::socketName(State);
    QCOMPARE(name, CallbackChannel::socketName(State));
    QVERIFY(name != CallbackChannel::socketName(OtherState));
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{24}$"));
    QVERIFY2(hex.match(name).hasMatch(), qPrintable(name));
}

void AvdCallbackTest::directoryIsOwnerOnly()
{
    CallbackChannel::DirectoryError error;
    const QString dir = CallbackChannel::directory(true, &error);
    QCOMPARE(error, CallbackChannel::DirectoryError::None);
    QVERIFY(dir.startsWith(qEnvironmentVariable("XDG_RUNTIME_DIR") + QLatin1Char('/')));
    const QFile::Permissions others = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    QCOMPARE(QFile::permissions(dir) & (ownerOnly() | others), ownerOnly());
}

void AvdCallbackTest::refusesInsecureDirectories()
{
    // A private directory of its own rather than one under the test's XDG_RUNTIME_DIR: that one lies in the
    // build directory, and deep builds would make every case below fail as PathTooLong before reaching
    // the check it is about.
    QTemporaryDir root(QDir::tempPath() + QStringLiteral("/krdc-cb-XXXXXX"));
    QVERIFY(root.isValid());
    QVERIFY(QFile::setPermissions(root.path(), ownerOnly()));
    // The deepest channel directory used for the Insecure cases, with a socket name, must fit sockaddr_un.
    QVERIFY2(QFile::encodeName(root.path() + QStringLiteral("/linked/krdc-avd/")).size() + 24 < qsizetype(sizeof(sockaddr_un::sun_path)),
             qPrintable(root.path()));
    CallbackChannel::DirectoryError error;
    {
        const EnvironmentOverride runtime("XDG_RUNTIME_DIR", QByteArray());
        QVERIFY(CallbackChannel::directory(true, &error).isEmpty());
        QCOMPARE(error, CallbackChannel::DirectoryError::NoRuntimeDirectory);
    }
    {
        const EnvironmentOverride runtime("XDG_RUNTIME_DIR", "relative/dir");
        QVERIFY(CallbackChannel::directory(true, &error).isEmpty());
        QCOMPARE(error, CallbackChannel::DirectoryError::NoRuntimeDirectory);
    }
    {
        // A shared runtime directory is not good enough.
        QVERIFY(QDir(root.path()).mkdir(QStringLiteral("shared")));
        QVERIFY(QFile::setPermissions(root.path() + QStringLiteral("/shared"), ownerOnly() | QFile::ReadOther | QFile::ExeOther));
        const EnvironmentOverride runtime("XDG_RUNTIME_DIR", (root.path() + QStringLiteral("/shared")).toUtf8());
        QVERIFY(CallbackChannel::directory(true, &error).isEmpty());
        QCOMPARE(error, CallbackChannel::DirectoryError::Insecure);
    }
    {
        // A channel directory others can enter, or a symbolic link, is not used.
        QVERIFY(QDir(root.path()).mkpath(QStringLiteral("open/krdc-avd")));
        QVERIFY(QFile::setPermissions(root.path() + QStringLiteral("/open"), ownerOnly()));
        QVERIFY(QFile::setPermissions(root.path() + QStringLiteral("/open/krdc-avd"), ownerOnly() | QFile::ExeGroup | QFile::ExeOther));
        const EnvironmentOverride runtime("XDG_RUNTIME_DIR", (root.path() + QStringLiteral("/open")).toUtf8());
        QVERIFY(CallbackChannel::directory(true, &error).isEmpty());
        QCOMPARE(error, CallbackChannel::DirectoryError::Insecure);
    }
    {
        QVERIFY(QDir(root.path()).mkpath(QStringLiteral("linked/target")));
        QVERIFY(QFile::setPermissions(root.path() + QStringLiteral("/linked"), ownerOnly()));
        QVERIFY(QFile::setPermissions(root.path() + QStringLiteral("/linked/target"), ownerOnly()));
        QVERIFY(QFile::link(root.path() + QStringLiteral("/linked/target"), root.path() + QStringLiteral("/linked/krdc-avd")));
        const EnvironmentOverride runtime("XDG_RUNTIME_DIR", (root.path() + QStringLiteral("/linked")).toUtf8());
        QVERIFY(CallbackChannel::directory(true, &error).isEmpty());
        QCOMPARE(error, CallbackChannel::DirectoryError::Insecure);
    }
    {
        // Socket paths have a fixed size limit.
        const QString deep = root.path() + QLatin1Char('/') + QString(100, QLatin1Char('d'));
        QVERIFY(QDir().mkpath(deep));
        QVERIFY(QFile::setPermissions(deep, ownerOnly()));
        const EnvironmentOverride runtime("XDG_RUNTIME_DIR", deep.toUtf8());
        QVERIFY(CallbackChannel::directory(true, &error).isEmpty());
        QCOMPARE(error, CallbackChannel::DirectoryError::PathTooLong);
    }
}

void AvdCallbackTest::handlerDeliversToLiveSignIn()
{
    CallbackReceiver receiver(Redirect, State);
    QSignalSpy received(&receiver, &CallbackReceiver::received);
    QSignalSpy failed(&receiver, &CallbackReceiver::failed);
    QString error;
    QVERIFY2(receiver.listen(&error), qPrintable(error));

    const HandlerRun run = runHandler(goodLink());
    QCOMPARE(run.exitCode, 0);
    QTRY_COMPARE(received.count(), 1);
    QCOMPARE(failed.count(), 0);
    const auto callback = received.first().first().value<AuthorizationCallback>();
    QCOMPARE(callback.code, QString::fromLatin1(Code));
    QCOMPARE(callback.state, State);
    // Nothing secret is printed, and the socket is gone.
    QVERIFY(!run.output.contains(Code));
    QVERIFY(!run.output.contains(State.toUtf8()));
    QVERIFY(!receiver.isListening());
    CallbackChannel::DirectoryError dirError;
    QVERIFY(QDir(CallbackChannel::directory(false, &dirError)).isEmpty(QDir::AllEntries | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot));
}

void AvdCallbackTest::handlerDoesNotReachOtherSignIns()
{
    CallbackReceiver receiver(Redirect, State);
    QSignalSpy received(&receiver, &CallbackReceiver::received);
    QSignalSpy failed(&receiver, &CallbackReceiver::failed);
    QString error;
    QVERIFY(receiver.listen(&error));

    const HandlerRun run = runHandler(goodLink(OtherState));
    QVERIFY(run.exitCode != 0);
    QVERIFY(!run.output.contains(Code));
    QTest::qWait(50);
    QCOMPARE(received.count(), 0);
    QCOMPARE(failed.count(), 0);
    QVERIFY(receiver.isListening());

    // The sign-in still gets its own answer afterwards.
    QCOMPARE(runHandler(goodLink()).exitCode, 0);
    QTRY_COMPARE(received.count(), 1);
}

void AvdCallbackTest::malformedLinkKeepsSignIn()
{
    CallbackReceiver receiver(Redirect, State);
    QSignalSpy received(&receiver, &CallbackReceiver::received);
    QSignalSpy failed(&receiver, &CallbackReceiver::failed);
    QString error;
    QVERIFY(receiver.listen(&error));

    // Any local program of the user can find the socket without knowing the
    // state, so a bad link must not end the sign-in the browser is finishing.
    const QByteArray wrongDestination =
        "ms-appx-web://Microsoft.AAD.BrokerPlugin/00000000-0000-0000-0000-000000000000?code=" + Code + "&state=" + State.toUtf8();
    QVERIFY(runHandler(wrongDestination).exitCode != 0);
    QVERIFY(runHandler(link("code=" + Code + "&state=" + State.toUtf8() + "&session_state=%zz")).exitCode != 0);
    QLocalSocket client;
    client.connectToServer(channelPath(State));
    QVERIFY(client.waitForConnected(1000));
    client.write("garbage\n");
    QTRY_COMPARE(client.state(), QLocalSocket::UnconnectedState);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(received.count(), 0);
    QVERIFY(receiver.isListening());

    QCOMPARE(runHandler(goodLink()).exitCode, 0);
    QTRY_COMPARE(received.count(), 1);
    QCOMPARE(failed.count(), 0);
}

void AvdCallbackTest::replayReachesNothing()
{
    CallbackReceiver receiver(Redirect, State);
    QSignalSpy received(&receiver, &CallbackReceiver::received);
    QSignalSpy failed(&receiver, &CallbackReceiver::failed);
    QString error;
    QVERIFY(receiver.listen(&error));

    QCOMPARE(runHandler(goodLink()).exitCode, 0);
    QTRY_COMPARE(received.count(), 1);
    QVERIFY(runHandler(goodLink()).exitCode != 0);
    QTest::qWait(50);
    QCOMPARE(received.count(), 1);
    QCOMPARE(failed.count(), 0);
}

void AvdCallbackTest::cancelInvalidatesCallbacks()
{
    CallbackReceiver receiver(Redirect, State);
    QSignalSpy received(&receiver, &CallbackReceiver::received);
    QSignalSpy failed(&receiver, &CallbackReceiver::failed);
    QString error;
    QVERIFY(receiver.listen(&error));
    receiver.cancel();
    QVERIFY(!receiver.isListening());

    QVERIFY(runHandler(goodLink()).exitCode != 0);
    QTest::qWait(50);
    QCOMPARE(received.count(), 0);
    QCOMPARE(failed.count(), 0);

    {
        // Destroying the receiver removes its socket too.
        CallbackReceiver other(Redirect, OtherState);
        QVERIFY(other.listen(&error));
    }
    QVERIFY(runHandler(goodLink(OtherState)).exitCode != 0);
}

void AvdCallbackTest::timesOut()
{
    CallbackReceiver receiver(Redirect, State);
    receiver.setTimeout(std::chrono::milliseconds(100));
    QSignalSpy failed(&receiver, &CallbackReceiver::failed);
    QString error;
    QVERIFY(receiver.listen(&error));
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(!receiver.isListening());
    QVERIFY(runHandler(goodLink()).exitCode != 0);
}

void AvdCallbackTest::cancelDropsPendingClients()
{
    CallbackReceiver receiver(Redirect, State);
    QSignalSpy received(&receiver, &CallbackReceiver::received);
    QSignalSpy failed(&receiver, &CallbackReceiver::failed);
    QString error;
    QVERIFY(receiver.listen(&error));
    QLocalSocket client;
    client.connectToServer(channelPath(State));
    QVERIFY(client.waitForConnected(1000));
    QTest::qWait(50);
    receiver.cancel();
    QTRY_COMPARE(client.state(), QLocalSocket::UnconnectedState);
    QCOMPARE(received.count(), 0);
    QCOMPARE(failed.count(), 0);
}

void AvdCallbackTest::boundsMessageSize()
{
    CallbackReceiver receiver(Redirect, State);
    QSignalSpy received(&receiver, &CallbackReceiver::received);
    QSignalSpy failed(&receiver, &CallbackReceiver::failed);
    QString error;
    QVERIFY(receiver.listen(&error));

    QLocalSocket client;
    client.connectToServer(channelPath(State));
    QVERIFY(client.waitForConnected(1000));
    // No newline within the limit: that client is dropped instead of buffering on.
    client.write(QByteArray(CallbackChannel::MaxMessageSize + 1, 'a'));
    QTRY_COMPARE(client.state(), QLocalSocket::UnconnectedState);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(received.count(), 0);
    QVERIFY(receiver.isListening());

    QCOMPARE(runHandler(goodLink()).exitCode, 0);
    QTRY_COMPARE(received.count(), 1);
}

void AvdCallbackTest::stalledClientDoesNotBlockSignIn()
{
    CallbackReceiver receiver(Redirect, State);
    receiver.setClientTimeout(std::chrono::milliseconds(300));
    QSignalSpy received(&receiver, &CallbackReceiver::received);
    QSignalSpy failed(&receiver, &CallbackReceiver::failed);
    QString error;
    QVERIFY(receiver.listen(&error));

    // Connected, half a link, then silence.
    QLocalSocket stalled;
    stalled.connectToServer(channelPath(State));
    QVERIFY(stalled.waitForConnected(1000));
    stalled.write("ms-appx-web://");
    stalled.flush();

    // The genuine link gets through while the stalled client still waits.
    QCOMPARE(runHandler(goodLink()).exitCode, 0);
    QTRY_COMPARE(received.count(), 1);
    QCOMPARE(failed.count(), 0);
    QTRY_COMPARE(stalled.state(), QLocalSocket::UnconnectedState);
}

void AvdCallbackTest::boundsConcurrentClients()
{
    CallbackReceiver receiver(Redirect, State);
    receiver.setClientTimeout(std::chrono::milliseconds(500));
    QSignalSpy received(&receiver, &CallbackReceiver::received);
    QSignalSpy failed(&receiver, &CallbackReceiver::failed);
    QString error;
    QVERIFY(receiver.listen(&error));

    std::vector<std::unique_ptr<QLocalSocket>> clients;
    for (int i = 0; i < CallbackReceiver::MaxClients + 1; ++i) {
        clients.push_back(std::make_unique<QLocalSocket>());
        clients.back()->connectToServer(channelPath(State));
        QVERIFY(clients.back()->waitForConnected(1000));
        QTest::qWait(20);
    }
    // One more than allowed is turned away at once, without waiting for the deadline.
    QTRY_COMPARE_WITH_TIMEOUT(clients.back()->state(), QLocalSocket::UnconnectedState, 300);
    for (int i = 0; i < CallbackReceiver::MaxClients; ++i) {
        QCOMPARE(clients.at(i)->state(), QLocalSocket::ConnectedState);
    }
    // Each stalled client is dropped at its own deadline; the sign-in goes on.
    for (int i = 0; i < CallbackReceiver::MaxClients; ++i) {
        QTRY_COMPARE(clients.at(i)->state(), QLocalSocket::UnconnectedState);
    }
    QCOMPARE(failed.count(), 0);
    QVERIFY(receiver.isListening());
    QCOMPARE(runHandler(goodLink()).exitCode, 0);
    QTRY_COMPARE(received.count(), 1);
}

void AvdCallbackTest::socketIsOwnerOnly()
{
    CallbackReceiver receiver(Redirect, State);
    QString error;
    QVERIFY(receiver.listen(&error));
    struct stat info;
    QCOMPARE(lstat(QFile::encodeName(channelPath(State)).constData(), &info), 0);
    QVERIFY(S_ISSOCK(info.st_mode));
    QCOMPARE(info.st_mode & (S_IRWXG | S_IRWXO), mode_t(0));
}

void AvdCallbackTest::handlerRefusesForeignLinks()
{
    CallbackReceiver receiver(Redirect, State);
    QSignalSpy received(&receiver, &CallbackReceiver::received);
    QSignalSpy failed(&receiver, &CallbackReceiver::failed);
    QString error;
    QVERIFY(receiver.listen(&error));

    const QList<QByteArray> arguments{QByteArray("https://login.microsoftonline.com/common/oauth2/nativeclient?code=c&state=" + State.toUtf8()),
                                      QByteArray("--help"),
                                      QByteArray(),
                                      QByteArray(goodLink() + "\nsecond line"),
                                      QByteArray(link("code=c&state=" + State.toUtf8()) + QByteArray(CallbackChannel::MaxMessageSize, 'a'))};
    for (const QByteArray &argument : arguments) {
        QVERIFY(runHandler(argument).exitCode != 0);
    }
    QTest::qWait(50);
    QCOMPARE(received.count(), 0);
    QCOMPARE(failed.count(), 0);
    QVERIFY(receiver.isListening());
}

QTEST_GUILESS_MAIN(AvdCallbackTest)

#include "avdcallbacktest.moc"
