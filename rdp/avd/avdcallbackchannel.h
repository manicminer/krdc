/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include <QByteArray>
#include <QString>

/**
 * The channel between krdc-avd-signin-handler, which the desktop starts for
 * ms-appx-web: links, and the KRDC sign-in waiting for one.
 *
 * Every waiting sign-in listens on its own socket in an owner-only directory
 * of XDG_RUNTIME_DIR. The socket is named after a hash of the sign-in's OAuth
 * state, so the handler reaches exactly the sign-in that started the browser
 * and nothing else; the name is always hexadecimal, whatever a link contains.
 * The handler writes the link and a newline and reads "OK" or "REJECTED".
 *
 * Shared by KRDC and the handler, so it has no dependencies beyond Qt Core.
 */
namespace Avd::CallbackChannel
{

/** Largest link accepted, including the newline that ends it. */
constexpr qsizetype MaxMessageSize = 16 * 1024;
/** How long either side waits for the other to write or answer. */
constexpr int IoTimeoutMs = 5000;

constexpr char BrokerScheme[] = "ms-appx-web";
constexpr char BrokerHost[] = "microsoft.aad.brokerplugin";

/** Whether @p state is an OAuth state KRDC or FreeRDP could have made. */
bool isValidState(const QString &state);

/**
 * The state of an ms-appx-web://Microsoft.AAD.BrokerPlugin/ link, if it has
 * exactly one valid state; otherwise an empty string. Only used to find the
 * socket: the receiving sign-in checks the whole link.
 */
QString stateOf(const QByteArray &link);

/** File name of the socket for @p state. */
QString socketName(const QString &state);

enum class DirectoryError {
    None,
    /** XDG_RUNTIME_DIR is unset or not absolute; there is no fallback to shared directories. */
    NoRuntimeDirectory,
    /** It, or the channel directory, is not a directory owned by and only accessible to this user. */
    Insecure,
    CannotCreate,
    /** Socket paths in it would be too long. */
    PathTooLong,
};

/**
 * The directory the channel directory is created in: @p runtimeDirectory, or,
 * inside a Flatpak sandbox (@p flatpakAppId not empty), its app/<id>
 * subdirectory. Flatpak gives each sandbox a private XDG_RUNTIME_DIR, so the
 * helper the desktop starts in a new sandbox of the same application could not
 * reach a socket there; app/<id> is the one runtime directory all sandboxes of
 * that application share with each other (and the user's host session). An
 * empty string if @p flatpakAppId is not a valid application ID.
 */
QString runtimeBase(const QString &runtimeDirectory, const QString &flatpakAppId);

/** The Flatpak application ID KRDC runs as, or an empty string outside a Flatpak sandbox. */
QString flatpakAppId();

/** The channel directory, after checking its owner and permissions; created if @p create. */
QString directory(bool create, DirectoryError *error);

/** Whether @p path is a socket owned by this user, not a symbolic link. */
bool isOwnedSocket(const QString &path);

/** Whether the peer of a connected local socket runs as this user. */
bool peerIsSameUser(qintptr socketDescriptor);

} // namespace Avd::CallbackChannel
