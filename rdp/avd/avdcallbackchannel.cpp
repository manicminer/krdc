/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdcallbackchannel.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>

#include <algorithm>
#include <cerrno>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace Avd::CallbackChannel
{

namespace
{
constexpr char DirectoryName[] = "krdc-avd";
constexpr int SocketNameLength = 24;

bool isStateCharacter(QChar c)
{
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
        || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char('.') || c == QLatin1Char('~');
}

// A directory owned by this user that nobody else can enter, not a symbolic link.
bool isPrivateDirectory(const QByteArray &path)
{
    struct stat info;
    if (lstat(path.constData(), &info) != 0) {
        return false;
    }
    return S_ISDIR(info.st_mode) && info.st_uid == geteuid() && (info.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}
}

bool isValidState(const QString &state)
{
    // KRDC's states have 32 random bytes in base64url, FreeRDP's in hex.
    if (state.size() < 16 || state.size() > 512) {
        return false;
    }
    return std::all_of(state.cbegin(), state.cend(), isStateCharacter);
}

QString stateOf(const QByteArray &link)
{
    const QByteArray prefix = QByteArray(BrokerScheme) + "://";
    // Schemes are case insensitive.
    if (link.size() >= MaxMessageSize || link.left(prefix.size()).compare(prefix, Qt::CaseInsensitive) != 0) {
        return {};
    }
    const qsizetype queryStart = link.indexOf('?');
    const QByteArray path = queryStart < 0 ? QByteArray() : link.mid(prefix.size(), queryStart - prefix.size());
    const qsizetype slash = path.indexOf('/');
    if (slash < 0 || path.left(slash).toLower() != BrokerHost) {
        return {};
    }
    QString state;
    int count = 0;
    QByteArray query = link.mid(queryStart + 1);
    const qsizetype fragment = query.indexOf('#');
    if (fragment >= 0) {
        query.truncate(fragment);
    }
    for (const QByteArray &item : query.split('&')) {
        if (item.startsWith("state=")) {
            ++count;
            state = QString::fromLatin1(item.mid(6));
        }
    }
    // Neither decoded nor guessed: valid states need no encoding.
    return count == 1 && isValidState(state) ? state : QString();
}

QString socketName(const QString &state)
{
    const QByteArray hash = QCryptographicHash::hash(QByteArray(QByteArray("krdc-avd-signin:") + state.toUtf8()), QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(hash.left(SocketNameLength));
}

QString directory(bool create, DirectoryError *error)
{
    const auto fail = [error](DirectoryError reason) {
        *error = reason;
        return QString();
    };
    *error = DirectoryError::None;

    // No fallback to /tmp: only the session's own runtime directory is private to the user.
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty() || !QDir::isAbsolutePath(runtime)) {
        return fail(DirectoryError::NoRuntimeDirectory);
    }
    if (!isPrivateDirectory(QFile::encodeName(runtime))) {
        return fail(DirectoryError::Insecure);
    }
    const QString path = QDir::cleanPath(runtime) + QLatin1Char('/') + QLatin1String(DirectoryName);
    const QByteArray encoded = QFile::encodeName(path);
    if (encoded.size() + 1 + SocketNameLength >= qsizetype(sizeof(sockaddr_un::sun_path))) {
        return fail(DirectoryError::PathTooLong);
    }
    if (create && mkdir(encoded.constData(), S_IRWXU) != 0 && errno != EEXIST) {
        return fail(DirectoryError::CannotCreate);
    }
    if (!isPrivateDirectory(encoded)) {
        return fail(create || QFile::exists(path) ? DirectoryError::Insecure : DirectoryError::CannotCreate);
    }
    return path;
}

bool isOwnedSocket(const QString &path)
{
    struct stat info;
    return lstat(QFile::encodeName(path).constData(), &info) == 0 && S_ISSOCK(info.st_mode) && info.st_uid == geteuid();
}

bool peerIsSameUser(qintptr socketDescriptor)
{
#if defined(SO_PEERCRED)
    struct ucred credentials;
    socklen_t size = sizeof(credentials);
    if (getsockopt(static_cast<int>(socketDescriptor), SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 || size != sizeof(credentials)) {
        return false;
    }
    return credentials.uid == geteuid();
#else
    uid_t uid;
    gid_t gid;
    return getpeereid(static_cast<int>(socketDescriptor), &uid, &gid) == 0 && uid == geteuid();
#endif
}

} // namespace Avd::CallbackChannel
