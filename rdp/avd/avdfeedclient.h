/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include "avdendpoints.h"
#include "avdfeed.h"

#include <QObject>
#include <QPointer>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace Avd
{

/**
 * Downloads the workspace feed and connection files.
 *
 * The bearer token is only sent to trusted HTTPS hosts. Redirects are
 * followed by this class, at most MaxRedirects times and only to trusted
 * hosts, and responses larger than the given limit are aborted.
 */
class FeedClient : public QObject
{
    Q_OBJECT

public:
    static constexpr int MaxRedirects = 5;

    FeedClient(QNetworkAccessManager *network, const CloudEndpoints &endpoints, QObject *parent = nullptr);

    /** Fetches the discovery document and every tenant feed. */
    void discover(const QString &accessToken);

    /** Fetches the connection file of @p resource. */
    void downloadConnectionFile(const Resource &resource, const QString &accessToken);

    /**
     * Fetches and decodes the icon of @p resource. Unlike feed requests,
     * redirects are refused. Failures are only logged: icons are optional.
     */
    void downloadIcon(const Resource &resource, const QString &accessToken);

    void abort();

Q_SIGNALS:
    /** @p complete is false if a tenant feed could not be loaded, so resources may be missing. */
    void resourcesReady(const QList<Avd::Resource> &resources, const QStringList &warnings, bool complete);
    void connectionFileReady(const Avd::Resource &resource, const QByteArray &contents);
    void iconReady(const QString &resourceKey, const QImage &icon);
    void failed(const QString &message);

private:
    using Handler = std::function<void(const QByteArray &body)>;
    using ErrorHandler = std::function<void(const QString &message)>;

    struct Limits {
        qsizetype maxSize;
        int maxRedirects = MaxRedirects;
        int timeout = 30000;
    };

    void get(const QUrl &url, const QByteArray &accept, const Limits &limits, int redirects, Handler onSuccess, ErrorHandler onError);
    void fetchNextFeed();

    QNetworkAccessManager *m_network;
    CloudEndpoints m_endpoints;
    QString m_token;
    QList<QPointer<QNetworkReply>> m_replies;
    QList<TenantFeed> m_pendingFeeds;
    QList<Resource> m_resources;
    QStringList m_warnings;
    bool m_complete = true;
    quint64 m_generation = 0;
};

} // namespace Avd
