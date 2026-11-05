/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include "avdendpoints.h"
#include "avdfeed.h"

#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QString>

#include <chrono>
#include <functional>
#include <optional>

namespace Avd
{

/**
 * Remembers the resources of a workspace feed per cloud and account, for at
 * most MaxAge after the fetch, so reopening the workspace window does not
 * fetch the feed again.
 *
 * The complete resources, with their connection file and icon addresses,
 * are only kept in memory. If a directory is given, metadata without any
 * address (titles, types, workspaces and decoded icons) is also kept there,
 * readable by the owner only, so the list survives restarts; desktops from
 * such an entry are found again in a fresh feed when they are opened.
 * Tokens are never stored.
 */
class WorkspaceCache
{
public:
    static constexpr std::chrono::seconds MaxAge{3600};
    static constexpr qsizetype MaxFileSize = 2 * 1024 * 1024;
    static constexpr int MaxResources = 1000;
    static constexpr int MaxTextLength = 512;
    static constexpr qsizetype MaxStoredIconSize = 64 * 1024;
    /** Icons are kept on disk at most this many pixels a side, */
    static constexpr int StoredIconDimension = 64;
    /** at most this many of them, */
    static constexpr int MaxStoredIcons = 200;
    /** and at most this much memory once decoded. */
    static constexpr qsizetype MaxStoredIconBytes = qsizetype(MaxStoredIcons) * StoredIconDimension * StoredIconDimension * 4;

    using Clock = std::function<QDateTime()>;

    struct Entry {
        QList<Resource> resources;
        QStringList warnings;
        /** When the feed was fetched; the age is not extended by reading the entry. */
        QDateTime fetchedAt;
        /** Decoded icons by Resource::key(). */
        QHash<QString, QImage> icons;
        /** False for entries read from disk: their resources have no addresses. */
        bool hasAddresses = true;
    };

    /** An empty @p directory keeps everything in memory only. */
    explicit WorkspaceCache(const QString &directory = QString());

    void setClock(Clock clock);
    QDateTime now() const;

    /** The entry, fresh or not, or nothing. */
    std::optional<Entry> lookup(const CloudEndpoints &cloud, const QString &account);
    /** Younger than MaxAge. Entries from the future, after the clock was set back, are not. */
    bool isFresh(const Entry &entry) const;

    /**
     * Changes whenever the entry of the account is removed. A fetch notes it
     * when it starts and hands it to store(), so a sign-out made meanwhile,
     * by whichever window, is not undone by the fetch's late answer.
     */
    quint64 generation(const CloudEndpoints &cloud, const QString &account) const;

    /**
     * Replaces the entry with the result of a complete fetch made now. Icons of resources still listed are kept.
     * Returns false, storing nothing, if the entry was removed since @p generation.
     */
    bool store(const CloudEndpoints &cloud,
               const QString &account,
               const QList<Resource> &resources,
               const QStringList &warnings,
               std::optional<quint64> generation = std::nullopt);
    void storeIcon(const CloudEndpoints &cloud,
                   const QString &account,
                   const QString &resourceKey,
                   const QImage &icon,
                   std::optional<quint64> generation = std::nullopt);
    void remove(const CloudEndpoints &cloud, const QString &account);

    /** The file of the entry; its name does not reveal the account. */
    QString filePath(const CloudEndpoints &cloud, const QString &account) const;

private:
    static QString key(const CloudEndpoints &cloud, const QString &account);
    std::optional<Entry> load(const QString &path) const;
    void save(const QString &path, const Entry &entry) const;

    QString m_directory;
    Clock m_clock;
    QHash<QString, Entry> m_entries;
    QHash<QString, quint64> m_generations;
};

/** Decodes a PNG or ICO icon of at most MaxIconFileSize bytes and @p maxDimension pixels a side. */
std::optional<QImage> decodeIcon(const QByteArray &data, QString *errorMessage = nullptr, int maxDimension = MaxIconDimension);

} // namespace Avd
