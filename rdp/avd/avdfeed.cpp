/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdfeed.h"

#include <KLocalizedString>

#include <QXmlStreamReader>

#include <iterator>

namespace Avd
{

namespace
{
bool checkSize(const QByteArray &xml, QString *errorMessage)
{
    if (xml.size() > MaxFeedDocumentSize) {
        if (errorMessage) {
            *errorMessage = i18nc("@info", "The workspace feed is larger than KRDC accepts.");
        }
        return false;
    }
    return true;
}

// QXmlStreamReader never fetches external entities; refusing DTDs also rules
// out internal entity expansion.
bool refuseDtd(QXmlStreamReader &reader, QString *errorMessage)
{
    if (reader.tokenType() == QXmlStreamReader::DTD || reader.tokenType() == QXmlStreamReader::EntityReference) {
        if (errorMessage) {
            *errorMessage = i18nc("@info", "The workspace feed contains a document type definition, which KRDC does not accept.");
        }
        return true;
    }
    return false;
}

bool finish(QXmlStreamReader &reader, QString *errorMessage)
{
    if (reader.hasError()) {
        if (errorMessage) {
            *errorMessage = i18nc("@info", "The workspace feed could not be read: %1", reader.errorString());
        }
        return false;
    }
    return true;
}

// [MS-TSWP] 2.2.2.1.6: Icon16 to Icon256 and IconRaw, each with FileType and FileURL; never inline.
int iconPreference(QStringView element, QStringView fileType)
{
    const bool png = fileType.compare(QLatin1String("Png"), Qt::CaseInsensitive) == 0;
    const bool ico = fileType.compare(QLatin1String("Ico"), Qt::CaseInsensitive) == 0;
    static const QLatin1String pngOrder[] = {QLatin1String("Icon64"),
                                             QLatin1String("Icon48"),
                                             QLatin1String("Icon32"),
                                             QLatin1String("Icon100"),
                                             QLatin1String("Icon256"),
                                             QLatin1String("Icon16")};
    if (png) {
        for (int i = 0; i < int(std::size(pngOrder)); ++i) {
            if (element == pngOrder[i]) {
                return int(std::size(pngOrder)) - i + 1;
            }
        }
    }
    if (ico && element == QLatin1String("IconRaw")) {
        return 1;
    }
    return 0;
}

// Icons get the bearer token too, so only the feed's own service may serve them:
// rdweb.wvd.microsoft.com feeds allow *.wvd.microsoft.com, not every trusted Microsoft host.
bool isTrustedIconUrl(const QUrl &url, const TenantFeed &tenant, const CloudEndpoints &endpoints)
{
    if (!isTrustedBearerUrl(url, endpoints)) {
        return false;
    }
    const QString feedHost = tenant.url.host().toLower();
    const QString service = feedHost.count(QLatin1Char('.')) >= 2 ? feedHost.section(QLatin1Char('.'), 1) : feedHost;
    return hostMatchesSuffix(url.host().toLower(), service);
}

Resource::Type typeFromString(QStringView type)
{
    if (type.compare(QLatin1String("Desktop"), Qt::CaseInsensitive) == 0) {
        return Resource::Type::Desktop;
    }
    if (type.compare(QLatin1String("RemoteApp"), Qt::CaseInsensitive) == 0) {
        return Resource::Type::RemoteApp;
    }
    return Resource::Type::Unknown;
}
}

bool Resource::isLaunchable() const
{
    return type == Type::Desktop && rdpFileUrl.isValid();
}

QString Resource::key() const
{
    return tenantId + QLatin1Char('/') + id;
}

std::optional<DiscoveryResult> parseDiscovery(const QByteArray &xml, const CloudEndpoints &endpoints, QString *errorMessage)
{
    if (!checkSize(xml, errorMessage)) {
        return std::nullopt;
    }

    DiscoveryResult result;
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (refuseDtd(reader, errorMessage)) {
            return std::nullopt;
        }
        if (!reader.isStartElement() || reader.name() != QLatin1String("TenantFeedURL")) {
            continue;
        }
        const auto attributes = reader.attributes();
        TenantFeed feed;
        feed.url = QUrl(attributes.value(QLatin1String("FeedURL")).toString(), QUrl::StrictMode);
        feed.tenantId = attributes.value(QLatin1String("TenantId")).toString();
        feed.tenantName = attributes.value(QLatin1String("TenantDisplayName")).toString();
        if (!isTrustedBearerUrl(feed.url, endpoints)) {
            result.warnings << i18nc("@info", "Skipped the workspaces of “%1”: their address is not a trusted Microsoft address.", feed.tenantName);
            continue;
        }
        result.feeds << feed;
    }
    if (!finish(reader, errorMessage)) {
        return std::nullopt;
    }
    return result;
}

std::optional<FeedResult> parseFeed(const QByteArray &xml, const TenantFeed &tenant, const CloudEndpoints &endpoints, QString *errorMessage)
{
    if (!checkSize(xml, errorMessage)) {
        return std::nullopt;
    }

    FeedResult result;
    QString workspace;
    std::optional<Resource> current;
    int iconRank = 0;
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (refuseDtd(reader, errorMessage)) {
            return std::nullopt;
        }
        if (reader.isStartElement()) {
            const auto attributes = reader.attributes();
            if (reader.name() == QLatin1String("Publisher")) {
                workspace = attributes.value(QLatin1String("Name")).toString();
            } else if (reader.name() == QLatin1String("Resource")) {
                Resource resource;
                resource.id = attributes.value(QLatin1String("ID")).toString();
                resource.title = attributes.value(QLatin1String("Title")).toString();
                resource.type = typeFromString(attributes.value(QLatin1String("Type")));
                resource.workspaceName = workspace;
                resource.tenantId = tenant.tenantId;
                resource.tenantName = tenant.tenantName;
                current = resource;
                iconRank = 0;
            } else if (current && reader.name().startsWith(QLatin1String("Icon")) && reader.name() != QLatin1String("Icons")) {
                const int rank = iconPreference(reader.name(), attributes.value(QLatin1String("FileType")));
                const QString file = attributes.value(QLatin1String("FileURL")).toString();
                if (rank > iconRank && !file.isEmpty()) {
                    // Relative in on-premises feeds; resolved against the feed, which is trusted.
                    const QUrl url = tenant.url.resolved(QUrl(file, QUrl::StrictMode));
                    if (isTrustedIconUrl(url, tenant, endpoints)) {
                        current->iconUrl = url;
                        iconRank = rank;
                    }
                }
            } else if (reader.name() == QLatin1String("ResourceFile") && current && current->rdpFileUrl.isEmpty()) {
                current->rdpFileUrl = QUrl(attributes.value(QLatin1String("URL")).toString(), QUrl::StrictMode);
            }
        } else if (reader.isEndElement() && reader.name() == QLatin1String("Resource") && current) {
            if (isTrustedBearerUrl(current->rdpFileUrl, endpoints)) {
                result.resources << *current;
            } else {
                result.warnings << i18nc("@info", "Skipped “%1”: its connection file is not at a trusted Microsoft address.", current->title);
            }
            current.reset();
        }
    }
    if (!finish(reader, errorMessage)) {
        return std::nullopt;
    }
    return result;
}

} // namespace Avd
