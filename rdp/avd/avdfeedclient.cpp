/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdfeedclient.h"
#include "avd/avdworkspacecache.h"
#include "krdc_rdpcore_debug.h"

#include <KLocalizedString>

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace Avd
{

namespace
{
constexpr qsizetype MaxConnectionFileSize = 256 * 1024;
constexpr int IconTimeout = 10000;
const QByteArray AcceptDiscovery = "application/x-msts-radc-discovery+xml,text/xml";
const QByteArray AcceptFeed = "application/x-msts-radc+xml;radc_schema_version=2.0,text/xml";
const QByteArray AcceptConnectionFile = "application/x-rdp,text/plain,*/*";
const QByteArray AcceptIcon = "image/png,image/x-icon,image/vnd.microsoft.icon";
}

FeedClient::FeedClient(QNetworkAccessManager *network, const CloudEndpoints &endpoints, QObject *parent)
    : QObject(parent)
    , m_network(network)
    , m_endpoints(endpoints)
{
}

void FeedClient::discover(const QString &accessToken)
{
    abort();
    m_token = accessToken;
    m_resources.clear();
    m_warnings.clear();
    m_complete = true;
    m_pendingFeeds.clear();

    get(
        m_endpoints.feedDiscoveryUrl,
        AcceptDiscovery,
        Limits{MaxFeedDocumentSize},
        0,
        [this](const QByteArray &body) {
            QString error;
            const auto discovery = parseDiscovery(body, m_endpoints, &error);
            if (!discovery) {
                Q_EMIT failed(error);
                return;
            }
            m_warnings << discovery->warnings;
            m_pendingFeeds = discovery->feeds;
            fetchNextFeed();
        },
        [this](const QString &message) {
            Q_EMIT failed(message);
        });
}

void FeedClient::fetchNextFeed()
{
    if (m_pendingFeeds.isEmpty()) {
        Q_EMIT resourcesReady(m_resources, m_warnings, m_complete);
        return;
    }
    const TenantFeed feed = m_pendingFeeds.takeFirst();
    get(
        feed.url,
        AcceptFeed,
        Limits{MaxFeedDocumentSize},
        0,
        [this, feed](const QByteArray &body) {
            QString error;
            const auto result = parseFeed(body, feed, m_endpoints, &error);
            if (result) {
                m_resources << result->resources;
                m_warnings << result->warnings;
            } else {
                m_complete = false;
                m_warnings << i18nc("@info", "The workspaces of “%1” could not be read: %2", feed.tenantName, error);
            }
            fetchNextFeed();
        },
        [this, feed](const QString &message) {
            m_complete = false;
            m_warnings << i18nc("@info", "The workspaces of “%1” could not be loaded: %2", feed.tenantName, message);
            fetchNextFeed();
        });
}

void FeedClient::downloadConnectionFile(const Resource &resource, const QString &accessToken)
{
    m_token = accessToken;
    if (!resource.isLaunchable()) {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                Q_EMIT failed(i18nc("@info", "KRDC can only open full desktops so far."));
            },
            Qt::QueuedConnection);
        return;
    }
    get(
        resource.rdpFileUrl,
        AcceptConnectionFile,
        Limits{MaxConnectionFileSize},
        0,
        [this, resource](const QByteArray &body) {
            Q_EMIT connectionFileReady(resource, body);
        },
        [this](const QString &message) {
            Q_EMIT failed(message);
        });
}

void FeedClient::downloadIcon(const Resource &resource, const QString &accessToken)
{
    if (resource.iconUrl.isEmpty()) {
        return;
    }
    m_token = accessToken;
    const QString key = resource.key();
    get(
        resource.iconUrl,
        AcceptIcon,
        Limits{MaxIconFileSize, 0, IconTimeout},
        0,
        [this, key](const QByteArray &body) {
            QString error;
            const auto icon = decodeIcon(body, &error);
            if (!icon) {
                qCDebug(KRDC_RDP) << "Ignoring the icon of a workspace resource:" << error;
                return;
            }
            Q_EMIT iconReady(key, *icon);
        },
        [](const QString &) {
            // Not the message: network errors can quote the icon's address, whose query may be a signature.
            qCDebug(KRDC_RDP) << "Could not load the icon of a workspace resource";
        });
}

void FeedClient::abort()
{
    ++m_generation;
    const auto replies = std::exchange(m_replies, {});
    for (const auto &reply : replies) {
        if (reply) {
            reply->abort();
        }
    }
}

void FeedClient::get(const QUrl &url, const QByteArray &accept, const Limits &limits, int redirects, Handler onSuccess, ErrorHandler onError)
{
    const qsizetype maxSize = limits.maxSize;
    const quint64 generation = m_generation;

    if (!isTrustedBearerUrl(url, m_endpoints)) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, onError, host = url.host()]() {
                if (generation == m_generation) {
                    onError(i18nc("@info", "KRDC does not send your sign-in to “%1”, which is not a trusted Microsoft address.", host));
                }
            },
            Qt::QueuedConnection);
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(limits.timeout);
    request.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    request.setRawHeader("X-MS-User-Agent", m_endpoints.msUserAgent.toUtf8());
    request.setRawHeader("Accept", accept);

    QNetworkReply *reply = m_network->get(request);
    m_replies.removeAll(nullptr);
    m_replies << reply;

    auto tooLarge = std::make_shared<bool>(false);
    const auto checkSize = [reply, maxSize, tooLarge]() {
        const qint64 announced = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (announced > maxSize || reply->bytesAvailable() > maxSize) {
            *tooLarge = true;
            reply->abort();
        }
    };
    connect(reply, &QNetworkReply::metaDataChanged, this, checkSize);
    connect(reply, &QNetworkReply::readyRead, this, checkSize);

    connect(reply, &QNetworkReply::finished, this, [=, this]() {
        reply->deleteLater();
        if (generation != m_generation) {
            return;
        }
        if (*tooLarge) {
            onError(i18nc("@info", "The workspace service sent more data than KRDC accepts."));
            return;
        }

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300 && status < 400) {
            const QUrl target = url.resolved(QUrl::fromEncoded(reply->rawHeader("Location"), QUrl::StrictMode));
            if (redirects >= limits.maxRedirects) {
                onError(i18nc("@info", "The workspace service redirected too often."));
            } else if (!isTrustedBearerUrl(target, m_endpoints)) {
                onError(i18nc("@info", "The workspace service redirected to “%1”, which is not a trusted Microsoft address.", target.host()));
            } else {
                get(target, accept, limits, redirects + 1, onSuccess, onError);
            }
            return;
        }
        if (status == 0 && reply->error() != QNetworkReply::NoError) {
            onError(i18nc("@info", "The workspace service could not be reached: %1", reply->errorString()));
            return;
        }
        if (status != 200) {
            onError(i18nc("@info", "The workspace service answered with HTTP status %1.", status));
            return;
        }
        const QByteArray body = reply->read(maxSize + 1);
        if (body.size() > maxSize) {
            onError(i18nc("@info", "The workspace service sent more data than KRDC accepts."));
            return;
        }
        onSuccess(body);
    });
}

} // namespace Avd
