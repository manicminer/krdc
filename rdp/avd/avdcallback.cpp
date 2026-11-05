/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdcallback.h"

#include "avd/avdcallbackchannel.h"

#include <KLocalizedString>

#include <QFileInfo>
#include <QHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

namespace Avd
{

namespace
{
bool isPrintableAscii(char c)
{
    return c > 0x20 && c < 0x7f;
}

// KIO, through which xdg-open passes links on Plasma, decodes %20 into spaces.
bool isLinkCharacter(char c)
{
    return c == ' ' || isPrintableAscii(c);
}

bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

enum class Controls {
    Refuse,
    /** For texts, such as error descriptions, which have line breaks. */
    MakeSpaces,
};

// Decodes %XX escapes, refusing broken ones.
// Decodes %XX escapes; parseAuthorizationCallback() already refused broken ones.
std::optional<QByteArray> decodeValue(const QByteArray &value, bool plusIsSpace, Controls controls)
{
    QByteArray decoded = QByteArray::fromPercentEncoding(plusIsSpace ? QByteArray(value).replace('+', ' ') : value);
    for (char &c : decoded) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) {
            if (controls == Controls::Refuse) {
                return std::nullopt;
            }
            c = ' ';
        }
    }
    return decoded;
}

bool equalInConstantTime(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        difference |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    }
    return difference == 0;
}

bool isKeyCharacter(char c)
{
    return (c >= 'a' && c <= 'z') || c == '_';
}

bool hasValidEscapes(const QByteArray &value)
{
    for (qsizetype i = 0; i < value.size(); ++i) {
        if (value.at(i) == '%' && (i + 2 >= value.size() || !isHexDigit(value.at(i + 1)) || !isHexDigit(value.at(i + 2)))) {
            return false;
        }
    }
    return true;
}

// Scheme and authority compare without case, as URIs define; the path, which
// carries the client ID, exactly. Without user info or port, which would
// change the authority.
bool isRedirect(const QByteArray &target, const QString &redirectUri)
{
    const QByteArray expected = redirectUri.toUtf8();
    const qsizetype expectedScheme = expected.indexOf("://");
    const qsizetype scheme = target.indexOf("://");
    if (expectedScheme <= 0 || scheme <= 0) {
        return false;
    }
    const qsizetype expectedPath = expected.indexOf('/', expectedScheme + 3);
    const qsizetype path = target.indexOf('/', scheme + 3);
    if (expectedPath < 0 || path < 0) {
        return false;
    }
    return target.left(scheme).compare(expected.left(expectedScheme), Qt::CaseInsensitive) == 0
        && target.mid(scheme + 3, path - scheme - 3).compare(expected.mid(expectedScheme + 3, expectedPath - expectedScheme - 3), Qt::CaseInsensitive) == 0
        && target.mid(path) == expected.mid(expectedPath);
}
}

QString brokerRedirectUri(const QString &clientId)
{
    return QStringLiteral("ms-appx-web://Microsoft.AAD.BrokerPlugin/") + clientId;
}

std::optional<AuthorizationCallback>
parseAuthorizationCallback(const QByteArray &link, const QString &redirectUri, const QString &expectedState, QString *errorMessage)
{
    const auto fail = [errorMessage](const QString &message) -> std::optional<AuthorizationCallback> {
        if (errorMessage) {
            *errorMessage = message;
        }
        return std::nullopt;
    };
    const QString malformed = i18nc("@info", "The sign-in answer is malformed.");

    if (link.size() >= CallbackChannel::MaxMessageSize) {
        return fail(i18nc("@info", "The sign-in answer is larger than KRDC accepts."));
    }
    if (!std::all_of(link.cbegin(), link.cend(), isLinkCharacter)) {
        return fail(malformed);
    }
    const qsizetype queryStart = link.indexOf('?');
    if (queryStart < 0 || link.contains('#')) {
        return fail(malformed);
    }
    // Nothing may be added to or left out of the redirect.
    if (!isRedirect(link.left(queryStart), redirectUri)) {
        return fail(i18nc("@info", "The sign-in answer was meant for another application."));
    }

    QHash<QByteArray, QByteArray> items;
    for (const QByteArray &item : link.mid(queryStart + 1).split('&')) {
        const qsizetype separator = item.indexOf('=');
        const QByteArray key = item.left(separator);
        const QByteArray value = item.mid(separator + 1);
        // Also the values KRDC does not use: a broken escape anywhere means a broken answer.
        if (separator <= 0 || !std::all_of(key.cbegin(), key.cend(), isKeyCharacter) || items.contains(key) || !hasValidEscapes(value)) {
            return fail(malformed);
        }
        items.insert(key, value);
    }

    if (expectedState.isEmpty() || !equalInConstantTime(items.value("state"), expectedState.toUtf8())) {
        return fail(i18nc("@info", "The sign-in answer does not belong to this sign-in."));
    }

    AuthorizationCallback callback;
    callback.state = expectedState;
    const bool hasCode = items.contains("code");
    const bool hasError = items.contains("error");
    if (hasCode == hasError) {
        return fail(malformed);
    }
    if (hasError) {
        const auto error = decodeValue(items.value("error"), true, Controls::Refuse);
        const auto description = decodeValue(items.value("error_description"), true, Controls::MakeSpaces);
        if (!error || error->isEmpty() || !description) {
            return fail(malformed);
        }
        callback.error = QString::fromUtf8(*error).left(200);
        callback.errorDescription = QString::fromUtf8(*description).simplified().left(500);
        return callback;
    }
    const auto code = decodeValue(items.value("code"), false, Controls::Refuse);
    if (!code || code->isEmpty() || !std::all_of(code->cbegin(), code->cend(), isPrintableAscii)) {
        return fail(malformed);
    }
    callback.code = QString::fromLatin1(*code);
    callback.encodedCode = items.value("code");
    return callback;
}

CallbackReceiver::CallbackReceiver(const QString &redirectUri, const QString &state, QObject *parent)
    : QObject(parent)
    , m_redirectUri(redirectUri)
    , m_state(state)
    , m_clientTimeout(CallbackChannel::IoTimeoutMs)
    , m_deadline(new QTimer(this))
{
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, [this]() {
        fail(i18nc("@info", "The sign-in in the browser was not finished in time. Sign in again."));
    });
}

CallbackReceiver::~CallbackReceiver()
{
    finish();
}

void CallbackReceiver::setTimeout(std::chrono::milliseconds timeout)
{
    m_timeout = timeout;
    if (m_deadline->isActive()) {
        m_deadline->start(m_timeout);
    }
}

void CallbackReceiver::setClientTimeout(std::chrono::milliseconds timeout)
{
    m_clientTimeout = timeout;
}

bool CallbackReceiver::listen(QString *errorMessage)
{
    if (m_server) {
        return true;
    }
    if (!CallbackChannel::isValidState(m_state)) {
        *errorMessage = i18nc("@info", "The sign-in could not be prepared.");
        return false;
    }
    CallbackChannel::DirectoryError error;
    const QString directory = CallbackChannel::directory(true, &error);
    switch (error) {
    case CallbackChannel::DirectoryError::None:
        break;
    case CallbackChannel::DirectoryError::NoRuntimeDirectory:
        *errorMessage = i18nc("@info", "The browser cannot return the sign-in to KRDC: the session has no private runtime directory (XDG_RUNTIME_DIR).");
        return false;
    case CallbackChannel::DirectoryError::Insecure:
        *errorMessage = i18nc("@info", "The browser cannot return the sign-in to KRDC: the runtime directory is accessible to other users.");
        return false;
    case CallbackChannel::DirectoryError::CannotCreate:
    case CallbackChannel::DirectoryError::PathTooLong:
        *errorMessage = i18nc("@info", "The browser cannot return the sign-in to KRDC: there is no place for it in the runtime directory.");
        return false;
    }

    m_path = directory + QLatin1Char('/') + CallbackChannel::socketName(m_state);
    m_server = new QLocalServer(this);
    // The directory is private already; the socket is too, independently of the umask.
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    m_server->setMaxPendingConnections(MaxClients + 1);
    // An existing socket is never removed: the name is new for every sign-in.
    if (QFileInfo::exists(m_path) || !m_server->listen(m_path)) {
        *errorMessage = i18nc("@info",
                              "The browser cannot return the sign-in to KRDC: %1",
                              m_server->errorString().isEmpty() ? i18nc("@info", "another sign-in uses the same channel.") : m_server->errorString());
        delete m_server;
        m_server = nullptr;
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, &CallbackReceiver::acceptConnection);
    m_deadline->start(m_timeout);
    return true;
}

bool CallbackReceiver::isListening() const
{
    return m_server;
}

void CallbackReceiver::cancel()
{
    finish();
}

void CallbackReceiver::acceptConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
        // Bounded, so that clients which never finish cannot use up memory or descriptors.
        if (m_clients.size() >= MaxClients || !CallbackChannel::peerIsSameUser(socket->socketDescriptor())) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_clients.insert(socket, QByteArray());
        auto *deadline = new QTimer(socket);
        deadline->setSingleShot(true);
        // Too slow: only this client is dropped; the browser's link may still come.
        connect(deadline, &QTimer::timeout, this, [this, socket]() {
            dropClient(socket);
        });
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
            readLink(socket);
        });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            // Gone without a whole link: the next one may still come.
            dropClient(socket);
        });
        deadline->start(m_clientTimeout);
        readLink(socket);
    }
}

void CallbackReceiver::readLink(QLocalSocket *client)
{
    const auto it = m_clients.find(client);
    if (it == m_clients.end()) {
        return;
    }
    QByteArray &buffer = *it;
    buffer += client->read(CallbackChannel::MaxMessageSize - buffer.size());
    const qsizetype end = buffer.indexOf('\n');
    if (end < 0) {
        if (buffer.size() >= CallbackChannel::MaxMessageSize) {
            dropClient(client, "REJECTED\n");
        }
        return;
    }

    QString error;
    const auto callback = parseAuthorizationCallback(buffer.left(end), m_redirectUri, m_state, &error);
    if (!callback) {
        // Any program of the user can reach the socket; what it sends ends nothing but its own attempt.
        dropClient(client, "REJECTED\n");
        return;
    }
    dropClient(client, "OK\n");
    finish();
    Q_EMIT received(*callback);
}

void CallbackReceiver::dropClient(QLocalSocket *client, const QByteArray &reply)
{
    const auto it = m_clients.find(client);
    if (it == m_clients.end()) {
        return;
    }
    it->fill('\0');
    m_clients.erase(it);
    client->disconnect(this);
    // Outlives the server, which owns it, until the reply is out.
    client->setParent(nullptr);
    if (reply.isEmpty()) {
        client->abort();
    } else {
        client->write(reply);
        client->flush();
        client->disconnectFromServer();
    }
    client->deleteLater();
}

void CallbackReceiver::finish()
{
    m_deadline->stop();
    const QList<QLocalSocket *> clients = m_clients.keys();
    for (QLocalSocket *client : clients) {
        dropClient(client);
    }
    if (m_server) {
        // Also removes the socket file.
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
}

void CallbackReceiver::fail(const QString &message)
{
    finish();
    Q_EMIT failed(message);
}

} // namespace Avd
