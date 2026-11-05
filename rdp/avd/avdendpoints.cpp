/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdendpoints.h"

#include <QHostAddress>

namespace Avd
{

QUrl CloudEndpoints::authorizeUrl() const
{
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(authorityHost);
    url.setPath(QLatin1Char('/') + tenant + QStringLiteral("/oauth2/v2.0/authorize"));
    return url;
}

QUrl CloudEndpoints::tokenUrl() const
{
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(authorityHost);
    url.setPath(QLatin1Char('/') + tenant + QStringLiteral("/oauth2/v2.0/token"));
    return url;
}

const CloudEndpoints &publicCloud()
{
    static const CloudEndpoints cloud{
        QStringLiteral("public"),
        QStringLiteral("login.microsoftonline.com"),
        QStringLiteral("organizations"),
        // Microsoft's public Remote Desktop client, also FreeRDP's default for AVD.
        QStringLiteral("a85cf173-4192-42f8-81fa-777a763e6e2c"),
        {QStringLiteral("https://www.wvd.microsoft.com/.default"), QStringLiteral("offline_access"), QStringLiteral("openid"), QStringLiteral("profile")},
        QUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feeddiscovery")),
        QStringLiteral("com.microsoft.rdc.html/2.0.79.2 rdhtml-sdk/2.0.4"),
        {QStringLiteral("microsoft.com"), QStringLiteral("microsoftonline.com")},
        {QStringLiteral("wvd.microsoft.com")},
    };
    return cloud;
}

bool hostMatchesSuffix(const QString &host, const QString &suffix)
{
    if (host.isEmpty() || suffix.isEmpty()) {
        return false;
    }
    const QString h = host.toLower();
    const QString s = suffix.toLower();
    return h == s || h.endsWith(QLatin1Char('.') + s);
}

bool isTrustedBearerUrl(const QUrl &url, const CloudEndpoints &endpoints)
{
    if (!url.isValid() || url.scheme() != QLatin1String("https") || !url.userInfo().isEmpty()) {
        return false;
    }
    const QString host = url.host();
    for (const QString &suffix : endpoints.trustedHostSuffixes) {
        if (hostMatchesSuffix(host, suffix)) {
            return true;
        }
    }
    return false;
}

bool isAvdGatewayHost(const QString &host, const CloudEndpoints &endpoints)
{
    for (const QString &suffix : endpoints.avdGatewayHostSuffixes) {
        if (hostMatchesSuffix(host, suffix)) {
            return true;
        }
    }
    return false;
}

bool isTrustedAvdGateway(const QString &host, int port, const CloudEndpoints &endpoints)
{
    if (port != -1 && port != 0 && port != 443) {
        return false;
    }
    if (QHostAddress(host).protocol() != QAbstractSocket::UnknownNetworkLayerProtocol) {
        return false;
    }
    return isAvdGatewayHost(host, endpoints);
}

} // namespace Avd
