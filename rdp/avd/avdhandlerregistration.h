/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include <QByteArray>
#include <QString>

namespace Avd
{

/**
 * Whether ms-appx-web: links, which end a Microsoft Entra sign-in in the
 * browser, are handed to krdc-avd-signin-handler, and making it so.
 *
 * Only one application handles a URI scheme for the user, so this replaces
 * whatever handled it before. KRDC only registers when the user agreed to
 * that, knowing the previous handler; nothing here is done implicitly.
 */
class HandlerRegistration
{
public:
    static constexpr char MimeType[] = "x-scheme-handler/ms-appx-web";
    static constexpr char DesktopFileName[] = "org.kde.krdc.avd-signin-handler.desktop";

    /**
     * @p handlerPath is the helper to register; see defaultHandlerPath().
     * @p packagedDesktopFile names the desktop file a package (the Flatpak)
     * installs for the helper; KRDC then neither checks nor writes any
     * registration, as only the desktop outside the sandbox decides which
     * application opens the links. Empty for native builds; see
     * packagedDesktopFileName().
     */
    explicit HandlerRegistration(const QString &handlerPath = defaultHandlerPath(), const QString &packagedDesktopFile = packagedDesktopFileName());

    /**
     * The desktop file the package installs for the helper (set at build time), when running inside
     * that package's Flatpak sandbox; otherwise, and always for native builds, an empty string.
     */
    static QString packagedDesktopFileName();

    /**
     * The helper next to the running executable, as in a build tree, or else
     * the installed one; an empty string if there is neither.
     */
    static QString defaultHandlerPath();

    enum class State {
        /** This KRDC's helper handles the links. */
        Registered,
        /** No application handles them. */
        NotRegistered,
        /** Another application handles them. */
        OtherHandler,
        /** KRDC's entry handles them, but with another helper, such as that of another build. */
        OtherKrdcHandler,
        /** The helper is missing or KRDC cannot safely register it. */
        HandlerUnusable,
    };

    struct Status {
        State state = State::NotRegistered;
        /** The application handling the links now, if any. */
        QString currentName;
        QString currentDesktopFile;
        QString currentCommand;
        /** Why the helper cannot be used, for HandlerUnusable. */
        QString problem;
    };

    Status status() const;

    /** Makes the helper the handler; only after the user agreed. */
    bool registerHandler(QString *errorMessage);

    QString handlerPath() const;

private:
    QString command() const;

    QString m_handlerPath;
    QString m_problem;
    QString m_packagedDesktopFile;
};

namespace detail
{
struct FileSnapshot {
    enum class Kind {
        Absent,
        Present,
        Unreadable
    };
    Kind kind = Kind::Absent;
    QByteArray contents;
};
FileSnapshot snapshotFile(const QString &path);
bool restoreFile(const QString &path, const FileSnapshot &snapshot);
}

} // namespace Avd
