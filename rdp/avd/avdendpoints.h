/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

namespace Avd
{

/**
 * Service endpoints and client identity used for Azure Virtual Desktop.
 *
 * Only the public cloud is defined. All other code reads endpoints from here
 * so that sovereign clouds can be added as further instances.
 *
 * KRDC is not a Microsoft client. It presents the public client ID and the
 * user agent of Microsoft's web client because the AVD feed rejects unknown
 * clients; Microsoft may change or block this at any time.
 */
struct CloudEndpoints {
    QString name;
    /** Host name of the Microsoft Entra ID authority. */
    QString authorityHost;
    /** Tenant path segment for the sign-in, for example "organizations". */
    QString tenant;
    QString clientId;
    /** Scopes requested for the workspace feed. */
    QStringList feedScopes;
    QUrl feedDiscoveryUrl;
    /** Value of the X-MS-User-Agent header the feed requires. */
    QString msUserAgent;
    /** Hosts (and their subdomains) that may receive the feed bearer token. */
    QStringList trustedHostSuffixes;
    /** Gateway hosts that identify an AVD connection. */
    QStringList avdGatewayHostSuffixes;

    QUrl authorizeUrl() const;
    QUrl tokenUrl() const;
};

const CloudEndpoints &publicCloud();

/** True if @p host is @p suffix or a subdomain of it, compared on label boundaries. */
bool hostMatchesSuffix(const QString &host, const QString &suffix);

/** True for an HTTPS URL on one of the trusted hosts, without user info. */
bool isTrustedBearerUrl(const QUrl &url, const CloudEndpoints &endpoints);

bool isAvdGatewayHost(const QString &host, const CloudEndpoints &endpoints);

/**
 * True if a gateway bearer token of @p endpoints may be sent to @p host and
 * @p port: an AVD gateway host of that cloud on the HTTPS port (-1 or 0 for the default).
 */
bool isTrustedAvdGateway(const QString &host, int port, const CloudEndpoints &endpoints);

} // namespace Avd
