/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdfeed.h"
#include "avd/avdfeedclient.h"
#include "fakenetwork.h"

#include <QSignalSpy>
#include <QTest>

using namespace Avd;

namespace
{
// Synthetic documents shaped like the AVD feed; tenants and names are made up.
const QUrl DiscoveryUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feeddiscovery"));
const QUrl FeedUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feed/webfeed?tenant=00000000-0000-0000-0000-0000000000aa"));
const QUrl DesktopRdpUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feed/prod/rdp/desktop-1.rdp"));

const QByteArray Discovery =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<TenantFeedURLs xmlns=\"http://schemas.microsoft.com/ts/2007/05/tswf\">\n"
    "  <TenantFeedURL FeedURL=\"https://rdweb.wvd.microsoft.com/api/arm/feed/webfeed?tenant=00000000-0000-0000-0000-0000000000aa\" "
    "TenantId=\"00000000-0000-0000-0000-0000000000aa\" TenantDisplayName=\"Example Org\"/>\n"
    "  <TenantFeedURL FeedURL=\"https://feed.example.org/webfeed\" TenantId=\"00000000-0000-0000-0000-0000000000bb\" TenantDisplayName=\"Untrusted\"/>\n"
    "  <TenantFeedURL FeedURL=\"http://rdweb.wvd.microsoft.com/plain\" TenantId=\"00000000-0000-0000-0000-0000000000cc\" TenantDisplayName=\"Plain\"/>\n"
    "</TenantFeedURLs>\n";

const QByteArray Feed =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<ResourceCollection PubDate=\"2026-01-01T00:00:00Z\" SchemaVersion=\"2.0\" xmlns=\"http://schemas.microsoft.com/ts/2007/05/tswf\">\n"
    "  <Publisher LastUpdated=\"2026-01-01T00:00:00Z\" Name=\"Example Workspace\" ID=\"pub-1\" Description=\"\">\n"
    "    <Resources>\n"
    "      <Resource ID=\"desktop-1\" Alias=\"desktop-1\" Title=\"Example Desktop\" LastUpdated=\"2026-01-01T00:00:00Z\" Type=\"Desktop\">\n"
    "        <Icons><Icon32 Dimensions=\"32x32\" FileType=\"Png\" FileURL=\"https://rdweb.wvd.microsoft.com/icon.png\"/></Icons>\n"
    "        <HostingTerminalServers><HostingTerminalServer>\n"
    "          <ResourceFile FileExtension=\".rdp\" URL=\"https://rdweb.wvd.microsoft.com/api/arm/feed/prod/rdp/desktop-1.rdp\"/>\n"
    "        </HostingTerminalServer></HostingTerminalServers>\n"
    "      </Resource>\n"
    "      <Resource ID=\"app-1\" Title=\"Example App\" Type=\"RemoteApp\">\n"
    "        <HostingTerminalServers><HostingTerminalServer>\n"
    "          <ResourceFile FileExtension=\".rdp\" URL=\"https://rdweb.wvd.microsoft.com/api/arm/feed/prod/rdp/app-1.rdp\"/>\n"
    "        </HostingTerminalServer></HostingTerminalServers>\n"
    "      </Resource>\n"
    "      <Resource ID=\"desktop-evil\" Title=\"Elsewhere\" Type=\"Desktop\">\n"
    "        <HostingTerminalServers><HostingTerminalServer>\n"
    "          <ResourceFile FileExtension=\".rdp\" URL=\"https://files.example.org/desktop.rdp\"/>\n"
    "        </HostingTerminalServer></HostingTerminalServers>\n"
    "      </Resource>\n"
    "    </Resources>\n"
    "  </Publisher>\n"
    "</ResourceCollection>\n";

const QByteArray EntityBomb =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE lolz [<!ENTITY lol \"lol\"><!ENTITY lol2 \"&lol;&lol;&lol;&lol;\">]>\n"
    "<TenantFeedURLs><TenantFeedURL FeedURL=\"&lol2;\"/></TenantFeedURLs>\n";

TenantFeed exampleTenant()
{
    return TenantFeed{FeedUrl, QStringLiteral("00000000-0000-0000-0000-0000000000aa"), QStringLiteral("Example Org")};
}
}

class AvdFeedTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void trustsOnlyHttpsMicrosoftHosts_data();
    void trustsOnlyHttpsMicrosoftHosts();
    void parsesDiscovery();
    void refusesDtd();
    void refusesMalformedAndOversized();
    void parsesFeed();
    void clientDiscoversResources();
    void clientFollowsTrustedRedirect();
    void clientRefusesUntrustedRedirect();
    void clientLimitsRedirects();
    void clientLimitsSize();
    void clientDoesNotSendTokenToUntrustedHost();
    void clientDownloadsConnectionFile();
    void clientReportsHttpErrorWithoutToken();
};

void AvdFeedTest::trustsOnlyHttpsMicrosoftHosts_data()
{
    QTest::addColumn<QString>("url");
    QTest::addColumn<bool>("trusted");

    QTest::newRow("feed") << QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feeddiscovery") << true;
    QTest::newRow("apex") << QStringLiteral("https://microsoft.com/") << true;
    QTest::newRow("login") << QStringLiteral("https://login.microsoftonline.com/x") << true;
    QTest::newRow("upper case") << QStringLiteral("https://RDWEB.WVD.Microsoft.COM/") << true;
    QTest::newRow("http") << QStringLiteral("http://rdweb.wvd.microsoft.com/") << false;
    QTest::newRow("lookalike") << QStringLiteral("https://evilmicrosoft.com/") << false;
    QTest::newRow("suffix trick") << QStringLiteral("https://microsoft.com.example.org/") << false;
    QTest::newRow("userinfo") << QStringLiteral("https://user@rdweb.wvd.microsoft.com/") << false;
    QTest::newRow("other") << QStringLiteral("https://example.org/") << false;
}

void AvdFeedTest::trustsOnlyHttpsMicrosoftHosts()
{
    QFETCH(QString, url);
    QFETCH(bool, trusted);
    QCOMPARE(isTrustedBearerUrl(QUrl(url), publicCloud()), trusted);
}

void AvdFeedTest::parsesDiscovery()
{
    QString error;
    const auto result = parseDiscovery(Discovery, publicCloud(), &error);
    QVERIFY2(result, qPrintable(error));
    QCOMPARE(result->feeds.size(), 1);
    QCOMPARE(result->feeds.first().url, FeedUrl);
    QCOMPARE(result->feeds.first().tenantName, QStringLiteral("Example Org"));
    QCOMPARE(result->warnings.size(), 2);
}

void AvdFeedTest::refusesDtd()
{
    QString error;
    QVERIFY(!parseDiscovery(EntityBomb, publicCloud(), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!parseFeed(EntityBomb, exampleTenant(), publicCloud(), &error));
}

void AvdFeedTest::refusesMalformedAndOversized()
{
    QString error;
    QVERIFY(!parseDiscovery("<TenantFeedURLs><TenantFeedURL", publicCloud(), &error));
    QVERIFY(!error.isEmpty());

    QByteArray big = Discovery;
    big.append(QByteArray(MaxFeedDocumentSize, ' '));
    QVERIFY(!parseDiscovery(big, publicCloud(), &error));
}

void AvdFeedTest::parsesFeed()
{
    QString error;
    const auto result = parseFeed(Feed, exampleTenant(), publicCloud(), &error);
    QVERIFY2(result, qPrintable(error));
    QCOMPARE(result->resources.size(), 2);

    const Resource desktop = result->resources.at(0);
    QCOMPARE(desktop.id, QStringLiteral("desktop-1"));
    QCOMPARE(desktop.title, QStringLiteral("Example Desktop"));
    QCOMPARE(desktop.type, Resource::Type::Desktop);
    QCOMPARE(desktop.workspaceName, QStringLiteral("Example Workspace"));
    QCOMPARE(desktop.tenantName, QStringLiteral("Example Org"));
    QCOMPARE(desktop.rdpFileUrl, DesktopRdpUrl);
    QVERIFY(desktop.isLaunchable());

    const Resource app = result->resources.at(1);
    QCOMPARE(app.type, Resource::Type::RemoteApp);
    QVERIFY(!app.isLaunchable());

    QCOMPARE(result->warnings.size(), 1);
}

void AvdFeedTest::clientDiscoversResources()
{
    FakeNetwork network;
    network.route(DiscoveryUrl, 200, Discovery);
    network.route(FeedUrl, 200, Feed);
    FeedClient client(&network, publicCloud());
    QSignalSpy ready(&client, &FeedClient::resourcesReady);
    QSignalSpy failed(&client, &FeedClient::failed);

    client.discover(QStringLiteral("synthetic-token"));
    QVERIFY(ready.wait());
    QCOMPARE(failed.count(), 0);
    const auto resources = ready.first().at(0).value<QList<Resource>>();
    QCOMPARE(resources.size(), 2);

    QCOMPARE(network.requests.size(), 2);
    for (const auto &recorded : std::as_const(network.requests)) {
        QCOMPARE(recorded.request.rawHeader("Authorization"), QByteArray("Bearer synthetic-token"));
        QCOMPARE(recorded.request.rawHeader("X-MS-User-Agent"), publicCloud().msUserAgent.toUtf8());
        QVERIFY(isTrustedBearerUrl(recorded.request.url(), publicCloud()));
        QCOMPARE(recorded.request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), int(QNetworkRequest::ManualRedirectPolicy));
    }
}

void AvdFeedTest::clientFollowsTrustedRedirect()
{
    FakeNetwork network;
    const QUrl moved(QStringLiteral("https://rdweb-r1.wvd.microsoft.com/api/arm/feeddiscovery"));
    network.route(DiscoveryUrl, 302, {}, {{"Location", moved.toEncoded()}});
    network.route(moved, 200, Discovery);
    network.route(FeedUrl, 200, Feed);
    FeedClient client(&network, publicCloud());
    QSignalSpy ready(&client, &FeedClient::resourcesReady);

    client.discover(QStringLiteral("synthetic-token"));
    QVERIFY(ready.wait());
    QCOMPARE(network.requests.at(1).request.url(), moved);
}

void AvdFeedTest::clientRefusesUntrustedRedirect()
{
    FakeNetwork network;
    network.route(DiscoveryUrl, 302, {}, {{"Location", "https://collector.example.org/steal"}});
    FeedClient client(&network, publicCloud());
    QSignalSpy failed(&client, &FeedClient::failed);

    client.discover(QStringLiteral("synthetic-token"));
    QVERIFY(failed.wait());
    QCOMPARE(network.requests.size(), 1);
    QVERIFY(!failed.first().at(0).toString().contains(QStringLiteral("synthetic-token")));
}

void AvdFeedTest::clientLimitsRedirects()
{
    FakeNetwork network;
    network.route(DiscoveryUrl, 302, {}, {{"Location", DiscoveryUrl.toEncoded()}});
    FeedClient client(&network, publicCloud());
    QSignalSpy failed(&client, &FeedClient::failed);

    client.discover(QStringLiteral("synthetic-token"));
    QVERIFY(failed.wait());
    QCOMPARE(network.requests.size(), FeedClient::MaxRedirects + 1);
}

void AvdFeedTest::clientLimitsSize()
{
    FakeNetwork network;
    network.route(DiscoveryUrl, 200, QByteArray(MaxFeedDocumentSize + 1, 'x'));
    FeedClient client(&network, publicCloud());
    QSignalSpy failed(&client, &FeedClient::failed);

    client.discover(QStringLiteral("synthetic-token"));
    QVERIFY(failed.wait());
}

void AvdFeedTest::clientDoesNotSendTokenToUntrustedHost()
{
    FakeNetwork network;
    FeedClient client(&network, publicCloud());
    QSignalSpy failed(&client, &FeedClient::failed);

    Resource resource;
    resource.id = QStringLiteral("x");
    resource.type = Resource::Type::Desktop;
    resource.rdpFileUrl = QUrl(QStringLiteral("https://files.example.org/desktop.rdp"));
    client.downloadConnectionFile(resource, QStringLiteral("synthetic-token"));
    QVERIFY(failed.wait());
    QVERIFY(network.requests.isEmpty());
}

void AvdFeedTest::clientDownloadsConnectionFile()
{
    FakeNetwork network;
    network.route(DesktopRdpUrl, 200, "full address:s:hostpool.example.org\r\n");
    FeedClient client(&network, publicCloud());
    QSignalSpy ready(&client, &FeedClient::connectionFileReady);

    Resource resource;
    resource.id = QStringLiteral("desktop-1");
    resource.type = Resource::Type::Desktop;
    resource.rdpFileUrl = DesktopRdpUrl;
    client.downloadConnectionFile(resource, QStringLiteral("synthetic-token"));
    QVERIFY(ready.wait());
    QCOMPARE(ready.first().at(1).toByteArray(), QByteArray("full address:s:hostpool.example.org\r\n"));
    QCOMPARE(ready.first().at(0).value<Resource>().id, QStringLiteral("desktop-1"));
}

void AvdFeedTest::clientReportsHttpErrorWithoutToken()
{
    FakeNetwork network;
    network.route(DiscoveryUrl, 401, "unauthorized synthetic-token");
    FeedClient client(&network, publicCloud());
    QSignalSpy failed(&client, &FeedClient::failed);

    client.discover(QStringLiteral("synthetic-token"));
    QVERIFY(failed.wait());
    const QString message = failed.first().at(0).toString();
    QVERIFY(message.contains(QStringLiteral("401")));
    QVERIFY(!message.contains(QStringLiteral("synthetic-token")));
}

QTEST_GUILESS_MAIN(AvdFeedTest)

#include "avdfeedtest.moc"
