/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdworkspacecache.h"
#include "avd/avdfeed.h"
#include "avd/avdfeedclient.h"
#include "fakenetwork.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace Avd;

namespace
{
const QUrl FeedUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feed/webfeed?tenant=t1"));
const QDateTime Start = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
const QString Alice = QStringLiteral("alice@example.org");
const QString Bob = QStringLiteral("bob@example.org");

Resource resource(const QString &id, Resource::Type type, const QString &title)
{
    Resource r;
    r.id = id;
    r.title = title;
    r.type = type;
    r.workspaceName = QStringLiteral("Example Workspace");
    r.tenantId = QStringLiteral("t1");
    r.tenantName = QStringLiteral("Example Org");
    r.rdpFileUrl = QUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feed/prod/rdp/%1.rdp?sig=SECRETSIG").arg(id));
    r.iconUrl = QUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feeddiscovery/tenants/t1/pngs/%1_32.png?sig=ICONSIG").arg(id));
    return r;
}

QList<Resource> sample()
{
    return {resource(QStringLiteral("desktop-1"), Resource::Type::Desktop, QStringLiteral("Engineering Desktop")),
            resource(QStringLiteral("app-1"), Resource::Type::RemoteApp, QStringLiteral("Spreadsheet"))};
}

CloudEndpoints otherCloud()
{
    CloudEndpoints cloud = publicCloud();
    cloud.name = QStringLiteral("Other cloud");
    cloud.authorityHost = QStringLiteral("login.example.us");
    return cloud;
}

QByteArray png(int width, int height)
{
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(Qt::darkCyan);
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return data;
}

// A cache file as another program could have left it; icons are PNGs by key.
QByteArray cacheFile(int resources, const QList<std::pair<QString, QByteArray>> &icons)
{
    QJsonArray list;
    for (int i = 0; i < resources; ++i) {
        list.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("app-%1").arg(i)},
                                {QStringLiteral("title"), QStringLiteral("App %1").arg(i)},
                                {QStringLiteral("type"), QStringLiteral("RemoteApp")},
                                {QStringLiteral("tenantId"), QStringLiteral("t1")}});
    }
    QJsonObject iconObject;
    for (const auto &[key, data] : icons) {
        iconObject.insert(key, QString::fromLatin1(data.toBase64()));
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                     {QStringLiteral("fetchedAt"), double(Start.toMSecsSinceEpoch())},
                                     {QStringLiteral("resources"), list},
                                     {QStringLiteral("icons"), iconObject}})
        .toJson(QJsonDocument::Compact);
}

TenantFeed tenant()
{
    return TenantFeed{FeedUrl, QStringLiteral("t1"), QStringLiteral("Example Org")};
}
}

class AvdWorkspaceCacheTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void freshUntilOneHourAfterFetch();
    void readingDoesNotExtendAge();
    void clockSetBackIsNotFresh();
    void isolatesAccountsAndClouds();
    void cachesEmptyResult();
    void removeForgetsAccountOnly();
    void removeOutdatesEarlierFetches();
    void keepsIconsOfListedResources();
    void persistsMetadataWithoutAddresses();
    void restoresAfterRestart();
    void refusesUnsafeFiles_data();
    void refusesUnsafeFiles();
    void parsesIconMetadata();
    void decodesOnlyBoundedIcons();
    void decodingLeavesGlobalAllocationLimit();
    void boundsIconsReadFromDisk();
    void clientDownloadsIcon();
    void clientRefusesIconRedirect();
    void clientSendsNoTokenToUntrustedIcon();

private:
    QDateTime m_now = Start;
    WorkspaceCache::Clock clock()
    {
        return [this]() {
            return m_now;
        };
    }
};

void AvdWorkspaceCacheTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void AvdWorkspaceCacheTest::freshUntilOneHourAfterFetch()
{
    m_now = Start;
    WorkspaceCache cache;
    cache.setClock(clock());
    QVERIFY(!cache.lookup(publicCloud(), Alice));
    cache.store(publicCloud(), Alice, sample(), {});

    m_now = Start.addSecs(3599);
    auto entry = cache.lookup(publicCloud(), Alice);
    QVERIFY(entry);
    QCOMPARE(entry->resources.size(), 2);
    QCOMPARE(entry->fetchedAt, Start);
    QVERIFY(entry->hasAddresses);
    QVERIFY(cache.isFresh(*entry));

    m_now = Start.addSecs(3600);
    entry = cache.lookup(publicCloud(), Alice);
    QVERIFY(entry);
    QVERIFY(!cache.isFresh(*entry));
}

void AvdWorkspaceCacheTest::readingDoesNotExtendAge()
{
    m_now = Start;
    WorkspaceCache cache;
    cache.setClock(clock());
    cache.store(publicCloud(), Alice, sample(), {});
    for (int minutes : {20, 40, 59}) {
        m_now = Start.addSecs(minutes * 60);
        QVERIFY(cache.isFresh(*cache.lookup(publicCloud(), Alice)));
    }
    m_now = Start.addSecs(3601);
    QVERIFY(!cache.isFresh(*cache.lookup(publicCloud(), Alice)));
}

void AvdWorkspaceCacheTest::clockSetBackIsNotFresh()
{
    m_now = Start;
    WorkspaceCache cache;
    cache.setClock(clock());
    cache.store(publicCloud(), Alice, sample(), {});
    m_now = Start.addSecs(-120);
    const auto entry = cache.lookup(publicCloud(), Alice);
    QVERIFY(entry);
    QVERIFY(!cache.isFresh(*entry));
}

void AvdWorkspaceCacheTest::isolatesAccountsAndClouds()
{
    m_now = Start;
    QTemporaryDir dir;
    WorkspaceCache cache(dir.path());
    cache.setClock(clock());
    cache.store(publicCloud(), Alice, sample(), {});
    QVERIFY(!cache.lookup(publicCloud(), Bob));
    QVERIFY(!cache.lookup(otherCloud(), Alice));
    // Entra ID account names are not case sensitive.
    QVERIFY(cache.lookup(publicCloud(), Alice.toUpper()));
    QVERIFY(cache.filePath(publicCloud(), Alice) != cache.filePath(otherCloud(), Alice));
    QVERIFY(cache.filePath(publicCloud(), Alice) != cache.filePath(publicCloud(), Bob));
    QVERIFY(!cache.filePath(publicCloud(), Alice).contains(QStringLiteral("alice")));

    WorkspaceCache restarted(dir.path());
    restarted.setClock(clock());
    QVERIFY(restarted.lookup(publicCloud(), Alice));
    QVERIFY(!restarted.lookup(publicCloud(), Bob));
    QVERIFY(!restarted.lookup(otherCloud(), Alice));
}

void AvdWorkspaceCacheTest::cachesEmptyResult()
{
    m_now = Start;
    QTemporaryDir dir;
    WorkspaceCache cache(dir.path());
    cache.setClock(clock());
    cache.store(publicCloud(), Alice, {}, {});
    auto entry = cache.lookup(publicCloud(), Alice);
    QVERIFY(entry);
    QVERIFY(entry->resources.isEmpty());
    QVERIFY(cache.isFresh(*entry));

    WorkspaceCache restarted(dir.path());
    restarted.setClock(clock());
    entry = restarted.lookup(publicCloud(), Alice);
    QVERIFY(entry);
    QVERIFY(entry->resources.isEmpty());
}

void AvdWorkspaceCacheTest::removeForgetsAccountOnly()
{
    m_now = Start;
    QTemporaryDir dir;
    WorkspaceCache cache(dir.path());
    cache.setClock(clock());
    cache.store(publicCloud(), Alice, sample(), {});
    cache.store(publicCloud(), Bob, sample(), {});
    QVERIFY(QFile::exists(cache.filePath(publicCloud(), Alice)));
    cache.remove(publicCloud(), Alice);
    QVERIFY(!cache.lookup(publicCloud(), Alice));
    QVERIFY(!QFile::exists(cache.filePath(publicCloud(), Alice)));
    QVERIFY(cache.lookup(publicCloud(), Bob));

    WorkspaceCache restarted(dir.path());
    QVERIFY(!restarted.lookup(publicCloud(), Alice));
}

void AvdWorkspaceCacheTest::removeOutdatesEarlierFetches()
{
    m_now = Start;
    QTemporaryDir dir;
    WorkspaceCache cache(dir.path());
    cache.setClock(clock());
    const auto resources = sample();
    QImage icon(32, 32, QImage::Format_ARGB32);
    icon.fill(Qt::red);

    // Two windows fetch for Alice; one of them signs her out before the other's answer arrives.
    const quint64 aliceFetch = cache.generation(publicCloud(), Alice);
    const quint64 bobFetch = cache.generation(publicCloud(), Bob);
    QVERIFY(cache.store(publicCloud(), Alice, resources, {}, aliceFetch));
    cache.remove(publicCloud(), Alice);
    QVERIFY(cache.generation(publicCloud(), Alice) != aliceFetch);
    QVERIFY(!cache.store(publicCloud(), Alice, resources, {}, aliceFetch));
    QVERIFY(!cache.lookup(publicCloud(), Alice));
    QVERIFY(!QFile::exists(cache.filePath(publicCloud(), Alice)));

    // A fetch started after the sign-out is stored; icons of the earlier one are not.
    const quint64 later = cache.generation(publicCloud(), Alice);
    QVERIFY(cache.store(publicCloud(), Alice, resources, {}, later));
    cache.storeIcon(publicCloud(), Alice, resources.at(1).key(), icon, aliceFetch);
    QVERIFY(cache.lookup(publicCloud(), Alice)->icons.isEmpty());
    cache.storeIcon(publicCloud(), Alice, resources.at(1).key(), icon, later);
    QCOMPARE(cache.lookup(publicCloud(), Alice)->icons.size(), 1);

    // Other accounts are not affected.
    QVERIFY(cache.store(publicCloud(), Bob, resources, {}, bobFetch));
}

void AvdWorkspaceCacheTest::keepsIconsOfListedResources()
{
    m_now = Start;
    QTemporaryDir dir;
    WorkspaceCache cache(dir.path());
    cache.setClock(clock());
    const auto resources = sample();
    cache.store(publicCloud(), Alice, resources, {});
    QImage icon(32, 32, QImage::Format_ARGB32);
    icon.fill(Qt::red);
    cache.storeIcon(publicCloud(), Alice, resources.at(1).key(), icon);
    cache.store(publicCloud(), Alice, resources, {});
    QCOMPARE(cache.lookup(publicCloud(), Alice)->icons.size(), 1);

    WorkspaceCache restarted(dir.path());
    restarted.setClock(clock());
    const auto entry = restarted.lookup(publicCloud(), Alice);
    QCOMPARE(entry->icons.size(), 1);
    QCOMPARE(entry->icons.value(resources.at(1).key()).size(), QSize(32, 32));

    // An app that left the feed takes its icon with it.
    cache.store(publicCloud(), Alice, {resources.at(0)}, {});
    QVERIFY(cache.lookup(publicCloud(), Alice)->icons.isEmpty());
}

void AvdWorkspaceCacheTest::persistsMetadataWithoutAddresses()
{
    m_now = Start;
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("avd"));
    WorkspaceCache cache(path);
    cache.setClock(clock());
    cache.store(publicCloud(), Alice, sample(), {});

    QFile file(cache.filePath(publicCloud(), Alice));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray contents = file.readAll();
    QVERIFY(contents.contains("Engineering Desktop"));
    for (const char *secret : {"SECRETSIG", "ICONSIG", "https://", "rdweb", ".rdp", "alice"}) {
        QVERIFY2(!contents.contains(secret), secret);
    }
    QCOMPARE(file.permissions() & ~(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser), QFile::Permissions());
    const QFileInfo directory(path);
    QCOMPARE(directory.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther),
             QFile::Permissions());
}

void AvdWorkspaceCacheTest::restoresAfterRestart()
{
    m_now = Start;
    QTemporaryDir dir;
    {
        WorkspaceCache cache(dir.path());
        cache.setClock(clock());
        cache.store(publicCloud(), Alice, sample(), {});
    }
    m_now = Start.addSecs(600);
    WorkspaceCache restarted(dir.path());
    restarted.setClock(clock());
    const auto entry = restarted.lookup(publicCloud(), Alice);
    QVERIFY(entry);
    QVERIFY(!entry->hasAddresses);
    QCOMPARE(entry->fetchedAt, Start);
    QVERIFY(restarted.isFresh(*entry));
    QCOMPARE(entry->resources.size(), 2);
    const Resource &desktop = entry->resources.at(0);
    QCOMPARE(desktop.title, QStringLiteral("Engineering Desktop"));
    QCOMPARE(desktop.type, Resource::Type::Desktop);
    QCOMPARE(desktop.workspaceName, QStringLiteral("Example Workspace"));
    QCOMPARE(desktop.key(), sample().at(0).key());
    QVERIFY(desktop.rdpFileUrl.isEmpty());
    QVERIFY(desktop.iconUrl.isEmpty());
    QCOMPARE(entry->resources.at(1).type, Resource::Type::RemoteApp);
}

void AvdWorkspaceCacheTest::refusesUnsafeFiles_data()
{
    QTest::addColumn<QByteArray>("contents");
    QTest::addColumn<bool>("loose");

    QTest::newRow("garbage") << QByteArray("{not json") << false;
    QTest::newRow("oversized") << QByteArray(WorkspaceCache::MaxFileSize + 1, ' ') << false;
    QTest::newRow("wrong version") << QByteArray(R"({"version":99,"fetchedAt":1767268800000,"resources":[]})") << false;
    QTest::newRow("too many")
        << (QByteArray(R"({"version":1,"fetchedAt":1767268800000,"resources":[)")
            + QByteArray(R"({"id":"x","tenantId":"t","title":"x","type":"Desktop"},)").repeated(WorkspaceCache::MaxResources + 1).chopped(1) + "]}")
        << false;
    QTest::newRow("long title") << (QByteArray(R"({"version":1,"fetchedAt":1767268800000,"resources":[{"id":"x","tenantId":"t","type":"Desktop","title":")")
                                    + QByteArray(WorkspaceCache::MaxTextLength + 1, 'a') + "\"}]}")
                                << false;
    QTest::newRow("readable by others") << QByteArray() << true;
}

void AvdWorkspaceCacheTest::refusesUnsafeFiles()
{
    QFETCH(QByteArray, contents);
    QFETCH(bool, loose);
    m_now = Start;
    QTemporaryDir dir;
    QString path;
    {
        WorkspaceCache cache(dir.path());
        cache.setClock(clock());
        cache.store(publicCloud(), Alice, sample(), {});
        path = cache.filePath(publicCloud(), Alice);
    }
    QFile file(path);
    if (loose) {
        QVERIFY(file.setPermissions(file.permissions() | QFile::ReadOther | QFile::ReadGroup));
    } else {
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(contents);
        file.close();
    }
    WorkspaceCache restarted(dir.path());
    restarted.setClock(clock());
    QVERIFY(!restarted.lookup(publicCloud(), Alice));
}

void AvdWorkspaceCacheTest::parsesIconMetadata()
{
    const QByteArray feed =
        "<ResourceCollection><Publisher Name=\"Example Workspace\"><Resources>"
        "<Resource ID=\"app-1\" Title=\"Spreadsheet\" Type=\"RemoteApp\"><Icons>"
        "<IconRaw FileType=\"Ico\" FileURL=\"/api/arm/feeddiscovery/tenants/t1/icons/app-1.ico?geo=EU\"/>"
        "<Icon32 Dimensions=\"32x32\" FileType=\"Png\" FileURL=\"/api/arm/feeddiscovery/tenants/t1/pngs/app-1_32_0.png?geo=EU\"/>"
        "</Icons><HostingTerminalServers><HostingTerminalServer>"
        "<ResourceFile FileExtension=\".rdp\" URL=\"https://rdweb.wvd.microsoft.com/api/arm/feed/prod/rdp/app-1.rdp\"/>"
        "</HostingTerminalServer></HostingTerminalServers></Resource>"
        "<Resource ID=\"app-2\" Title=\"Ico Only\" Type=\"RemoteApp\"><Icons>"
        "<IconRaw FileType=\"Ico\" FileURL=\"https://rdweb.wvd.microsoft.com/api/arm/icons/app-2.ico\"/>"
        "</Icons><HostingTerminalServers><HostingTerminalServer>"
        "<ResourceFile FileExtension=\".rdp\" URL=\"https://rdweb.wvd.microsoft.com/api/arm/feed/prod/rdp/app-2.rdp\"/>"
        "</HostingTerminalServer></HostingTerminalServers></Resource>"
        "<Resource ID=\"app-3\" Title=\"Elsewhere\" Type=\"RemoteApp\"><Icons>"
        "<Icon32 Dimensions=\"32x32\" FileType=\"Png\" FileURL=\"https://collector.example.org/app.png\"/>"
        "<Icon48 Dimensions=\"48x48\" FileType=\"Png\" FileURL=\"http://rdweb.wvd.microsoft.com/app.png\"/>"
        "<Icon64 Dimensions=\"64x64\" FileType=\"Svg\" FileURL=\"https://rdweb.wvd.microsoft.com/app.svg\"/>"
        "</Icons><HostingTerminalServers><HostingTerminalServer>"
        "<ResourceFile FileExtension=\".rdp\" URL=\"https://rdweb.wvd.microsoft.com/api/arm/feed/prod/rdp/app-3.rdp\"/>"
        "</HostingTerminalServer></HostingTerminalServers></Resource>"
        "<Resource ID=\"app-4\" Title=\"Other Service\" Type=\"RemoteApp\"><Icons>"
        "<Icon32 Dimensions=\"32x32\" FileType=\"Png\" FileURL=\"https://login.microsoftonline.com/app.png\"/>"
        "<Icon48 Dimensions=\"48x48\" FileType=\"Png\" FileURL=\"https://www.microsoft.com/app.png\"/>"
        "<Icon64 Dimensions=\"64x64\" FileType=\"Png\" FileURL=\"https://user@rdweb.wvd.microsoft.com/app.png\"/>"
        "</Icons><HostingTerminalServers><HostingTerminalServer>"
        "<ResourceFile FileExtension=\".rdp\" URL=\"https://rdweb.wvd.microsoft.com/api/arm/feed/prod/rdp/app-4.rdp\"/>"
        "</HostingTerminalServer></HostingTerminalServers></Resource>"
        "</Resources></Publisher></ResourceCollection>";
    QString error;
    const auto result = parseFeed(feed, tenant(), publicCloud(), &error);
    QVERIFY2(result, qPrintable(error));
    QCOMPARE(result->resources.size(), 4);
    // Trusted for the token, but another service than the feed's: not for icons.
    QVERIFY(result->resources.at(3).iconUrl.isEmpty());
    QCOMPARE(result->resources.at(0).iconUrl,
             QUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feeddiscovery/tenants/t1/pngs/app-1_32_0.png?geo=EU")));
    QCOMPARE(result->resources.at(1).iconUrl, QUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/icons/app-2.ico")));
    // Untrusted, plain HTTP and SVG icons are dropped; the resource stays.
    QVERIFY(result->resources.at(2).iconUrl.isEmpty());
}

void AvdWorkspaceCacheTest::decodesOnlyBoundedIcons()
{
    const auto good = decodeIcon(png(32, 32));
    QVERIFY(good);
    QCOMPARE(good->size(), QSize(32, 32));

    QVERIFY(!decodeIcon(png(MaxIconDimension + 1, 16)));
    QVERIFY(!decodeIcon(QByteArray("not an image")));
    QVERIFY(!decodeIcon(QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\"><image href=\"https://example.org/x.png\"/></svg>")));
    QByteArray huge = png(32, 32);
    huge.append(QByteArray(MaxIconFileSize, '\0'));
    QVERIFY(!decodeIcon(huge));
    QVERIFY(!decodeIcon(QByteArray()));
}

void AvdWorkspaceCacheTest::decodingLeavesGlobalAllocationLimit()
{
    // The limit is process-wide: decoding a feed icon must not change it for the rest of KRDC.
    const int original = QImageReader::allocationLimit();
    const auto restore = qScopeGuard([original]() {
        QImageReader::setAllocationLimit(original);
    });
    QImageReader::setAllocationLimit(321);
    QVERIFY(decodeIcon(png(32, 32)));
    QVERIFY(!decodeIcon(png(MaxIconDimension + 1, 16)));
    QCOMPARE(QImageReader::allocationLimit(), 321);
}

void AvdWorkspaceCacheTest::boundsIconsReadFromDisk()
{
    m_now = Start;
    QTemporaryDir dir;
    QString path;
    {
        WorkspaceCache cache(dir.path());
        cache.setClock(clock());
        cache.store(publicCloud(), Alice, sample(), {});
        path = cache.filePath(publicCloud(), Alice);
    }
    const auto rewrite = [&path](const QByteArray &contents) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size();
    };
    const auto load = [this, &dir]() {
        WorkspaceCache restarted(dir.path());
        restarted.setClock(clock());
        return restarted.lookup(publicCloud(), Alice);
    };

    // Icons of resources the file does not list, and icons larger than KRDC stores, are not decoded.
    QVERIFY(rewrite(cacheFile(2,
                              {{QStringLiteral("t1/app-0"), png(32, 32)},
                               {QStringLiteral("t1/app-1"), png(WorkspaceCache::StoredIconDimension + 1, 16)},
                               {QStringLiteral("t1/orphan"), png(32, 32)},
                               {QStringLiteral("t9/app-0"), png(32, 32)}})));
    auto entry = load();
    QVERIFY(entry);
    QCOMPARE(entry->resources.size(), 2);
    QCOMPARE(entry->icons.keys(), QStringList{QStringLiteral("t1/app-0")});

    // However compact the file, the number of icons and their decoded size are bounded.
    const QByteArray tiny = png(WorkspaceCache::StoredIconDimension, WorkspaceCache::StoredIconDimension);
    QList<std::pair<QString, QByteArray>> many;
    const int count = WorkspaceCache::MaxStoredIcons + 50;
    for (int i = 0; i < count; ++i) {
        many.append({QStringLiteral("t1/app-%1").arg(i), tiny});
    }
    const QByteArray contents = cacheFile(count, many);
    QVERIFY(contents.size() <= WorkspaceCache::MaxFileSize);
    QVERIFY(rewrite(contents));
    entry = load();
    QVERIFY(entry);
    QCOMPARE(entry->resources.size(), count);
    QVERIFY(entry->icons.size() <= WorkspaceCache::MaxStoredIcons);
    qsizetype decoded = 0;
    for (const QImage &icon : std::as_const(entry->icons)) {
        QVERIFY(icon.width() <= WorkspaceCache::StoredIconDimension && icon.height() <= WorkspaceCache::StoredIconDimension);
        decoded += icon.sizeInBytes();
    }
    QVERIFY(decoded <= WorkspaceCache::MaxStoredIconBytes);
}

void AvdWorkspaceCacheTest::clientDownloadsIcon()
{
    FakeNetwork network;
    Resource app = sample().at(1);
    network.route(app.iconUrl, 200, png(48, 48));
    FeedClient client(&network, publicCloud());
    QSignalSpy ready(&client, &FeedClient::iconReady);
    QSignalSpy failed(&client, &FeedClient::failed);
    client.downloadIcon(app, QStringLiteral("synthetic-token"));
    QVERIFY(ready.wait());
    QCOMPARE(ready.first().at(0).toString(), app.key());
    QCOMPARE(ready.first().at(1).value<QImage>().size(), QSize(48, 48));
    QCOMPARE(failed.count(), 0);
    QCOMPARE(network.requests.size(), 1);
    QCOMPARE(network.requests.first().request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), int(QNetworkRequest::ManualRedirectPolicy));

    // Too large: announced or sent.
    network.requests.clear();
    network.route(app.iconUrl, 200, QByteArray(MaxIconFileSize + 1, 'x'));
    client.downloadIcon(app, QStringLiteral("synthetic-token"));
    QVERIFY(!ready.wait(300));
    QCOMPARE(failed.count(), 0);
}

void AvdWorkspaceCacheTest::clientRefusesIconRedirect()
{
    FakeNetwork network;
    Resource app = sample().at(1);
    const QUrl moved(QStringLiteral("https://rdweb-r1.wvd.microsoft.com/icon.png"));
    network.route(app.iconUrl, 302, {}, {{"Location", moved.toEncoded()}});
    network.route(moved, 200, png(32, 32));
    FeedClient client(&network, publicCloud());
    QSignalSpy ready(&client, &FeedClient::iconReady);
    QSignalSpy failed(&client, &FeedClient::failed);
    client.downloadIcon(app, QStringLiteral("synthetic-token"));
    QVERIFY(!ready.wait(300));
    QCOMPARE(network.requests.size(), 1);
    QCOMPARE(failed.count(), 0);
}

void AvdWorkspaceCacheTest::clientSendsNoTokenToUntrustedIcon()
{
    FakeNetwork network;
    Resource app = sample().at(1);
    app.iconUrl = QUrl(QStringLiteral("https://collector.example.org/icon.png"));
    network.route(app.iconUrl, 200, png(32, 32));
    FeedClient client(&network, publicCloud());
    QSignalSpy ready(&client, &FeedClient::iconReady);
    client.downloadIcon(app, QStringLiteral("synthetic-token"));
    QVERIFY(!ready.wait(300));
    QVERIFY(network.requests.isEmpty());
}

QTEST_MAIN(AvdWorkspaceCacheTest)

#include "avdworkspacecachetest.moc"
