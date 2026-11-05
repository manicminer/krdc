/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include "avdendpoints.h"

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace Avd
{

/** Largest feed document KRDC parses. */
constexpr qsizetype MaxFeedDocumentSize = 4 * 1024 * 1024;

struct TenantFeed {
    QUrl url;
    QString tenantId;
    QString tenantName;
};

struct Resource {
    enum class Type {
        Desktop,
        RemoteApp,
        Unknown,
    };

    QString id;
    QString title;
    Type type = Type::Unknown;
    QString workspaceName;
    QString tenantId;
    QString tenantName;
    QUrl rdpFileUrl;
    /** PNG or ICO icon the feed offers, only kept if at a trusted address. Not persisted: it may carry a query. */
    QUrl iconUrl;

    /** Only full desktops can be opened so far. */
    bool isLaunchable() const;
    /** Identifies the resource across feed fetches. */
    QString key() const;
};

struct DiscoveryResult {
    QList<TenantFeed> feeds;
    /** Entries that were skipped, for example because of an untrusted URL. */
    QStringList warnings;
};

struct FeedResult {
    QList<Resource> resources;
    QStringList warnings;
};

/** Parses the feed discovery document. Documents with a DTD are refused. */
std::optional<DiscoveryResult> parseDiscovery(const QByteArray &xml, const CloudEndpoints &endpoints, QString *errorMessage);

/** Icons KRDC downloads, see FeedClient::downloadIcon(). */
constexpr qsizetype MaxIconFileSize = 256 * 1024;
constexpr int MaxIconDimension = 256;

/** Parses a tenant feed document. Documents with a DTD are refused. */
std::optional<FeedResult> parseFeed(const QByteArray &xml, const TenantFeed &tenant, const CloudEndpoints &endpoints, QString *errorMessage);

} // namespace Avd

Q_DECLARE_METATYPE(Avd::Resource)
