/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdhandlerregistration.h"

#include <KApplicationTrader>
#include <KLocalizedString>
#include <KService>
#include <KSycoca>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>

#include <functional>
#include <memory>
#include <optional>

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace Avd
{

namespace
{
constexpr char HandlerName[] = "krdc-avd-signin-handler";

// Paths made of these need no quoting or escaping on an Exec line.
bool isPlainPathCharacter(QChar c)
{
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
        || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('/') || c == QLatin1Char('.') || c == QLatin1Char('_') || c == QLatin1Char('-')
        || c == QLatin1Char('+');
}

bool isOwnedByRootOrUser(const struct stat &info)
{
    return info.st_uid == 0 || info.st_uid == geteuid();
}

// Whoever may change the helper, or rename the folder it is in or any folder
// above, can put another program in its place, which would then get the
// sign-ins. Folders like /tmp are fine: their sticky bit lets others rename
// only what they own. @p canonical has no symbolic links.
bool isTrustworthyHelper(const QString &canonical)
{
    struct stat info;
    if (lstat(QFile::encodeName(canonical).constData(), &info) != 0 || !S_ISREG(info.st_mode) || !isOwnedByRootOrUser(info)
        || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return false;
    }
    QString folder = canonical;
    do {
        const qsizetype slash = folder.lastIndexOf(QLatin1Char('/'));
        folder = slash <= 0 ? QStringLiteral("/") : folder.left(slash);
        if (lstat(QFile::encodeName(folder).constData(), &info) != 0 || !S_ISDIR(info.st_mode) || !isOwnedByRootOrUser(info)) {
            return false;
        }
        if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0 && (info.st_mode & S_ISVTX) == 0) {
            return false;
        }
    } while (folder != QLatin1String("/"));
    return true;
}

QString mimeappsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/mimeapps.list");
}

QString desktopFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation) + QLatin1Char('/') + QLatin1String(HandlerRegistration::DesktopFileName);
}

// KSycoca looks at the files at most every 1.5 seconds in each thread, too
// rarely to see a change just made; in a new thread it looks at once.
template<typename T>
T withCurrentServices(const std::function<T()> &query)
{
    T result{};
    std::unique_ptr<QThread> thread(QThread::create([&result, &query]() {
        KSycoca::disableAutoRebuild();
        KSycoca::self()->ensureCacheValid();
        result = query();
    }));
    thread->start();
    thread->wait();
    return result;
}
}

namespace detail
{
FileSnapshot snapshotFile(const QString &path)
{
    struct stat info;
    if (lstat(QFile::encodeName(path).constData(), &info) != 0) {
        // Only a file known to be missing is absent; anything else cannot be put back.
        return {errno == ENOENT ? FileSnapshot::Kind::Absent : FileSnapshot::Kind::Unreadable, {}};
    }
    // A symbolic link, as dotfile managers make, would be replaced by a file.
    QFile file(path);
    if (!S_ISREG(info.st_mode) || !file.open(QIODevice::ReadOnly)) {
        return {FileSnapshot::Kind::Unreadable, {}};
    }
    QByteArray contents = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return {FileSnapshot::Kind::Unreadable, {}};
    }
    return {FileSnapshot::Kind::Present, contents};
}

bool restoreFile(const QString &path, const FileSnapshot &snapshot)
{
    const FileSnapshot current = snapshotFile(path);
    switch (snapshot.kind) {
    case FileSnapshot::Kind::Absent:
        return current.kind == FileSnapshot::Kind::Absent || QFile::remove(path);
    case FileSnapshot::Kind::Present: {
        if (current.kind == FileSnapshot::Kind::Present && current.contents == snapshot.contents) {
            return true;
        }
        QSaveFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(snapshot.contents) == snapshot.contents.size() && file.commit();
    }
    case FileSnapshot::Kind::Unreadable:
        break;
    }
    return false;
}
}

HandlerRegistration::HandlerRegistration(const QString &handlerPath)
{
    const QFileInfo info(handlerPath);
    const QString canonical = handlerPath.isEmpty() ? QString() : info.canonicalFilePath();
    if (canonical.isEmpty()) {
        m_problem = i18nc("@info", "KRDC's sign-in helper, %1, is not installed.", QLatin1String(HandlerName));
    } else if (!info.isFile() || !info.isExecutable()) {
        m_problem = i18nc("@info", "KRDC's sign-in helper, %1, cannot be run.", canonical);
    } else if (!isTrustworthyHelper(canonical)) {
        m_problem = i18nc("@info", "KRDC's sign-in helper, %1, or a folder it is in can be changed by other users, so KRDC does not register it.", canonical);
    } else if (!std::all_of(canonical.cbegin(), canonical.cend(), isPlainPathCharacter)) {
        m_problem = i18nc("@info",
                          "The path of KRDC's sign-in helper, %1, contains characters KRDC does not register. "
                          "Install KRDC, or build it in a folder whose path has only letters, digits and ./_-+.",
                          canonical);
    } else {
        m_handlerPath = canonical;
    }
}

QString HandlerRegistration::defaultHandlerPath()
{
    // A build tree keeps its helper next to krdc; an installed KRDC in libexec.
    const QString beside = QCoreApplication::applicationDirPath() + QLatin1Char('/') + QLatin1String(HandlerName);
    if (QFileInfo(beside).isExecutable()) {
        return beside;
    }
    const QString installed = QStringLiteral(KRDC_AVD_HANDLER_INSTALL_PATH);
    if (QFileInfo(installed).isExecutable()) {
        return installed;
    }
    return {};
}

QString HandlerRegistration::handlerPath() const
{
    return m_handlerPath;
}

QString HandlerRegistration::command() const
{
    return m_handlerPath + QStringLiteral(" %u");
}

HandlerRegistration::Status HandlerRegistration::status() const
{
    Status status = withCurrentServices<Status>([]() {
        Status current;
        if (const KService::Ptr service = KApplicationTrader::preferredService(QLatin1String(MimeType))) {
            current.currentName = service->name();
            current.currentDesktopFile = service->storageId();
            current.currentCommand = service->exec();
        }
        return current;
    });
    if (!m_problem.isEmpty()) {
        status.state = State::HandlerUnusable;
        status.problem = m_problem;
    } else if (status.currentDesktopFile.isEmpty()) {
        status.state = State::NotRegistered;
    } else if (status.currentDesktopFile == QLatin1String(DesktopFileName)) {
        status.state = status.currentCommand == command() ? State::Registered : State::OtherKrdcHandler;
    } else {
        status.state = State::OtherHandler;
    }
    return status;
}

bool HandlerRegistration::registerHandler(QString *errorMessage)
{
    if (!m_problem.isEmpty()) {
        *errorMessage = m_problem;
        return false;
    }
    // Checked again: the user may have taken a while to agree.
    const HandlerRegistration current(m_handlerPath);
    if (!current.m_problem.isEmpty() || current.m_handlerPath != m_handlerPath) {
        *errorMessage =
            current.m_problem.isEmpty() ? i18nc("@info", "KRDC's sign-in helper, %1, changed; open this dialog again.", m_handlerPath) : current.m_problem;
        return false;
    }

    // Nothing is changed unless both files can be put back as they were.
    const QString desktopPath = desktopFilePath();
    const detail::FileSnapshot previousDesktopFile = detail::snapshotFile(desktopPath);
    const detail::FileSnapshot previousMimeapps = detail::snapshotFile(mimeappsPath());
    for (const auto &[path, snapshot] : {std::pair{desktopPath, previousDesktopFile}, std::pair{mimeappsPath(), previousMimeapps}}) {
        if (snapshot.kind == detail::FileSnapshot::Kind::Unreadable) {
            *errorMessage = i18nc("@info", "KRDC changed nothing: it cannot read %1, so it could not restore it if registering failed.", path);
            return false;
        }
    }
    const auto undo = [&]() {
        QStringList notRestored;
        if (!detail::restoreFile(desktopPath, previousDesktopFile)) {
            notRestored << desktopPath;
        }
        if (!detail::restoreFile(mimeappsPath(), previousMimeapps)) {
            notRestored << mimeappsPath();
        }
        if (!notRestored.isEmpty()) {
            *errorMessage +=
                QLatin1Char(' ') + i18nc("@info", "KRDC could not undo its changes to %1; check these files.", notRestored.join(QStringLiteral(", ")));
        }
    };

    QDir().mkpath(QFileInfo(desktopPath).absolutePath());
    QSaveFile file(desktopPath);
    if (!file.open(QIODevice::WriteOnly)) {
        *errorMessage = i18nc("@info", "KRDC could not write %1: %2", desktopPath, file.errorString());
        return false;
    }
    // Handles nothing but ms-appx-web: links, and is not shown in menus.
    file.write("[Desktop Entry]\n"
               "Type=Application\n"
               "Name="
               + i18nc("@title name of a desktop entry", "KRDC Microsoft Entra Sign-in").toUtf8()
               + "\n"
                 "Comment="
               + i18nc("@info desktop entry comment", "Returns Microsoft Entra sign-ins from the browser to KRDC").toUtf8()
               + "\n"
                 "Exec="
               + command().toUtf8()
               + "\n"
                 "Icon=krdc\n"
                 "NoDisplay=true\n"
                 "Terminal=false\n"
                 "StartupNotify=false\n"
                 "MimeType="
               + QByteArray(MimeType) + ";\n");
    if (!file.commit()) {
        *errorMessage = i18nc("@info", "KRDC could not write %1: %2", desktopPath, file.errorString());
        undo();
        return false;
    }

    const bool preferred = withCurrentServices<bool>([this]() {
        const KService::Ptr service = KService::serviceByStorageId(QLatin1String(DesktopFileName));
        if (!service || service->exec() != command()) {
            return false;
        }
        KApplicationTrader::setPreferredService(QLatin1String(MimeType), service);
        return true;
    });
    if (!preferred) {
        *errorMessage = i18nc("@info", "KRDC could not add its sign-in helper to the applications of the desktop.");
        undo();
        return false;
    }

    const Status after = status();
    if (after.state != State::Registered) {
        *errorMessage = i18nc("@info",
                              "ms-appx-web: links still open with “%1”: a setting of the desktop takes precedence over KRDC's choice. "
                              "Change it in the file associations of the desktop.",
                              after.currentName.isEmpty() ? i18nc("@info no application", "no application") : after.currentName);
        undo();
        return false;
    }
    return true;
}

} // namespace Avd
