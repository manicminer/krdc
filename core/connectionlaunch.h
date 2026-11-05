/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#ifndef CONNECTIONLAUNCH_H
#define CONNECTIONLAUNCH_H

#include "krdccore_export.h"

#include <QHash>
#include <QMetaType>
#include <QString>
#include <QUrl>

#include <memory>

/**
 * Backend-specific launch data that cannot be expressed as a URL, such as the
 * settings of an imported connection file. It lives in memory only: it is
 * never written to bookmarks, history or the list of open sessions.
 */
class KRDCCORE_EXPORT LaunchPayload
{
public:
    virtual ~LaunchPayload();

    /** Identifies the payload type, for example "rdp-file". */
    virtual QString kind() const = 0;
};

/**
 * Everything needed to start a connection. The URL identifies the connection
 * and is shown to the user; it must not carry secrets.
 */
class KRDCCORE_EXPORT ConnectionLaunch
{
public:
    static ConnectionLaunch fromUrl(const QUrl &url);

    bool isValid() const;

    QUrl url;
    /** Suggested tab name; empty uses the URL. */
    QString name;
    std::shared_ptr<const LaunchPayload> payload;
    /**
     * False for launches whose URL alone cannot reproduce the connection, so
     * they are kept out of history and restored sessions.
     */
    bool remember = true;
    /** True for launches loaded from a connection file, whose fileSettings are untrusted. */
    bool fromFile = false;
    /** For RemoteViewFactory::importFileSettings(). */
    QHash<QString, QString> fileSettings;
};

Q_DECLARE_METATYPE(ConnectionLaunch)

#endif // CONNECTIONLAUNCH_H
