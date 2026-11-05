/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdworkspacedialog.h"
#include "avd/avdresourceview.h"
#include "avd/avdtokenstore.h"
#include "avd/avdworkspacecache.h"
#include "fakenetwork.h"
#include "rdplaunchprofile.h"

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QToolTip>
#include <QUrlQuery>

using namespace Avd;

namespace
{
// Synthetic feed; every name is made up.
const QUrl DiscoveryUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feeddiscovery"));
const QUrl FeedUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feed/webfeed?tenant=t1"));
const QUrl DesktopRdpUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feed/prod/rdp/desktop-1.rdp"));
const QString Alice = QStringLiteral("alice@example.org");
const QString LongTitle = QStringLiteral("Quarterly Financial Reporting and Consolidation Workbench for the Example Regional Subsidiaries");
const QDateTime Start = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);

const QByteArray Discovery =
    "<TenantFeedURLs><TenantFeedURL FeedURL=\"https://rdweb.wvd.microsoft.com/api/arm/feed/webfeed?tenant=t1\" TenantId=\"t1\" "
    "TenantDisplayName=\"Example Org\"/></TenantFeedURLs>";

QByteArray resourceXml(const QString &id, const QString &title, const char *type)
{
    return QStringLiteral(
               "<Resource ID=\"%1\" Title=\"%2\" Type=\"%3\"><HostingTerminalServers><HostingTerminalServer>"
               "<ResourceFile FileExtension=\".rdp\" URL=\"https://rdweb.wvd.microsoft.com/api/arm/feed/prod/rdp/%1.rdp\"/>"
               "</HostingTerminalServer></HostingTerminalServers></Resource>")
        .arg(id, title, QLatin1String(type))
        .toUtf8();
}

QByteArray feed(const QList<QByteArray> &resources)
{
    return "<ResourceCollection><Publisher Name=\"Example Workspace\"><Resources>" + resources.join() + "</Resources></Publisher></ResourceCollection>";
}

const QByteArray Feed = feed({resourceXml(QStringLiteral("desktop-1"), QStringLiteral("Engineering Desktop"), "Desktop"),
                              resourceXml(QStringLiteral("app-1"), QStringLiteral("Spreadsheet"), "RemoteApp")});
const QByteArray DesktopRdp =
    "full address:s:hostpool.example.org\r\ngatewayhostname:s:rdgateway.wvd.microsoft.com\r\nenablerdsaadauth:i:1\r\nresourceprovider:s:arm\r\n";

class CountingTokenStore : public TokenStore
{
public:
    void read(const QString &account, ReadCallback callback) override
    {
        ++reads;
        callback(tokens.value(account));
    }
    void write(const QString &account, const QString &refreshToken) override
    {
        tokens.insert(account, refreshToken);
    }
    void remove(const QString &account) override
    {
        tokens.remove(account);
    }
    QHash<QString, QString> tokens;
    int reads = 0;
};

QByteArray tokenJson(const QString &access, const QString &refresh)
{
    // id_token for alice@example.org, unsigned: KRDC only reads it for a label.
    return QStringLiteral(R"({"access_token":"%1","refresh_token":"%2","expires_in":3600,)"
                          R"("id_token":"eyJhbGciOiJub25lIn0.eyJwcmVmZXJyZWRfdXNlcm5hbWUiOiJhbGljZUBleGFtcGxlLm9yZyJ9."})")
        .arg(access, refresh)
        .toUtf8();
}

std::optional<AuthorizationCallback> answerWithCode(const AuthorizationRequest &request)
{
    AuthorizationCallback callback;
    callback.code = QStringLiteral("c");
    callback.encodedCode = "c";
    callback.state = request.state;
    return callback;
}

void routeAll(FakeNetwork &network, const QByteArray &feedXml = Feed)
{
    network.route(DiscoveryUrl, 200, Discovery);
    network.route(FeedUrl, 200, feedXml);
    network.route(DesktopRdpUrl, 200, DesktopRdp);
    network.route(publicCloud().tokenUrl(), 200, tokenJson(QStringLiteral("access-1"), QStringLiteral("refresh-1")));
}

QList<QUrl> urls(const FakeNetwork &network, int from = 0)
{
    QList<QUrl> result;
    for (int i = from; i < network.requests.size(); ++i) {
        result << network.requests.at(i).request.url();
    }
    return result;
}

// WCAG contrast of an icon's visible pixels against a background.
qreal iconContrast(const QIcon &icon, const QColor &background)
{
    const QImage image = icon.pixmap(22, 22).toImage().convertToFormat(QImage::Format_ARGB32);
    qreal luminance = 0;
    qreal weight = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            luminance += color.alphaF() * (0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF());
            weight += color.alphaF();
        }
    }
    if (weight == 0) {
        return 0;
    }
    const qreal icon_ = luminance / weight;
    const qreal back = 0.2126 * background.redF() + 0.7152 * background.greenF() + 0.0722 * background.blueF();
    return (std::max(icon_, back) + 0.05) / (std::min(icon_, back) + 0.05);
}

QPalette palette(bool dark)
{
    QPalette p;
    const QColor window = dark ? QColor(0x20, 0x23, 0x26) : QColor(0xef, 0xf0, 0xf1);
    const QColor text = dark ? QColor(0xfc, 0xfc, 0xfc) : QColor(0x23, 0x26, 0x29);
    for (auto role : {QPalette::Window, QPalette::Button, QPalette::Base, QPalette::AlternateBase}) {
        p.setColor(role, window);
    }
    for (auto role : {QPalette::WindowText, QPalette::ButtonText, QPalette::Text}) {
        p.setColor(role, text);
    }
    return p;
}

bool isMagenta(QRgb pixel)
{
    return qRed(pixel) > 230 && qGreen(pixel) < 25 && qBlue(pixel) > 230;
}

// A desktop card painted by the delegate alone, at a given device pixel ratio.
struct PaintedCard {
    QImage image;
    QRect art;
};

PaintedCard paintDesktopCard(const QImage &serverImage, qreal dpr)
{
    Resource desktop;
    desktop.id = QStringLiteral("desktop-1");
    desktop.title = QStringLiteral("Engineering Desktop");
    desktop.type = Resource::Type::Desktop;
    AvdResourceModel model;
    model.setResources({desktop}, {{desktop.key(), serverImage}});
    const QRect rect(0, 0, 360, 104);
    QImage image((QSizeF(rect.size()) * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::white);
    QStyleOptionViewItem option;
    option.rect = rect;
    option.state = QStyle::State_Enabled;
    QPainter painter(&image);
    AvdResourceDelegate().paint(&painter, option, model.index(0, 0));
    painter.end();
    const QRect content =
        rect.adjusted(AvdResourceView::Gap / 2 + 12, AvdResourceView::Gap / 2 + 12, -AvdResourceView::Gap / 2 - 12, -AvdResourceView::Gap / 2 - 12);
    return {image, QRect(content.left(), content.top(), qRound(content.height() * 1.5), content.height())};
}

// The magenta part of an image, in logical pixels.
QRectF magentaBounds(const QImage &image)
{
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (isMagenta(image.pixel(x, y))) {
                bounds |= QRect(x, y, 1, 1);
            }
        }
    }
    const qreal dpr = image.devicePixelRatio();
    return QRectF(bounds.x() / dpr, bounds.y() / dpr, bounds.width() / dpr, bounds.height() / dpr);
}

QModelIndex rowWithTitle(const AvdResourceView *view, const QString &title)
{
    const QAbstractItemModel *model = view->model();
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->index(row, 0).data().toString() == title) {
            return model->index(row, 0);
        }
    }
    return {};
}

// The width QListView wraps rows at: the viewport at its widest, less a vertical scroll bar whether or not
// one shows and, for styles that frame only the contents, that frame.
int listWrapWidth(const QListView *view)
{
    QStyle *style = view->style();
    int width = view->maximumViewportSize().width() - style->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, view->verticalScrollBar());
    if (style->styleHint(QStyle::SH_ScrollView_FrameOnlyAroundContents, nullptr, view)) {
        QStyleOption option;
        option.initFrom(view);
        width -= 2 * style->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, view);
    }
    return width;
}

// What is wrong with the cards as actually laid out, or nothing.
QString gridProblem(const AvdResourceView *view)
{
    const QAbstractItemModel *model = view->model();
    if (!model || model->rowCount() == 0) {
        return {};
    }
    const int minWidth = view->category() == AvdResourceModel::Category::Apps ? AvdResourceView::MinAppWidth : AvdResourceView::MinDeviceWidth;
    const int wrap = listWrapWidth(view);
    const int columns = view->columnCount();
    const int top = view->visualRect(model->index(0, 0)).top();
    int firstRow = 0;
    int right = 0;
    for (int row = 0; row < model->rowCount(); ++row) {
        const QRect rect = view->visualRect(model->index(row, 0));
        if (rect.size() != view->cardSize() || rect.left() < 0 || rect.right() >= view->viewport()->width()) {
            return QStringLiteral("card %1 at %2,%3 %4x%5 does not fit a viewport %6 wide")
                .arg(row)
                .arg(rect.x())
                .arg(rect.y())
                .arg(rect.width())
                .arg(rect.height())
                .arg(view->viewport()->width());
        }
        if (rect.top() == top) {
            ++firstRow;
            right = rect.right();
        }
    }
    const QString state = QStringLiteral(" (viewport %1, wrap %2, scroll bar %3, card %4)")
                              .arg(view->viewport()->width())
                              .arg(wrap)
                              .arg(view->verticalScrollBar()->isVisible())
                              .arg(view->cardSize().width());
    if (view->horizontalScrollBar()->maximum() != 0) {
        return QStringLiteral("scrolls sideways") + state;
    }
    const int fit = std::max(1, (wrap - 1) / minWidth);
    if (columns != fit) {
        return QStringLiteral("%1 columns where %2 fit").arg(columns).arg(fit) + state;
    }
    if (firstRow != std::min(columns, model->rowCount())) {
        return QStringLiteral("%1 columns counted but %2 cards in the first row").arg(columns).arg(firstRow) + state;
    }
    // The cards share out the width, all but the rounding, unless they are as wide as they get.
    const int unused = wrap - 1 - (right + 1);
    if (view->cardSize().width() < AvdResourceView::MaxCardWidth + AvdResourceView::Gap && firstRow == columns && unused >= columns) {
        return QStringLiteral("%1 pixels left over beside the first row").arg(unused) + state;
    }
    return {};
}

QList<Resource> syntheticApps(int count)
{
    QList<Resource> resources;
    for (int i = 0; i < count; ++i) {
        Resource app;
        app.id = QStringLiteral("app-%1").arg(i);
        app.tenantId = QStringLiteral("t1");
        app.title = QStringLiteral("Example App %1").arg(i);
        app.workspaceName = QStringLiteral("Example Workspace");
        app.type = Resource::Type::RemoteApp;
        resources << app;
    }
    return resources;
}

// Styles to lay the cards out in; ones not installed are skipped.
void addStyleRows()
{
    QTest::addColumn<QString>("style");
    QTest::addColumn<bool>("dark");
    for (const char *style : {"Fusion", "Oxygen", "Breeze"}) {
        QTest::addRow("%s-light", style) << QString::fromLatin1(style) << false;
        QTest::addRow("%s-dark", style) << QString::fromLatin1(style) << true;
    }
}

QPalette darkPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(0x20, 0x23, 0x26));
    palette.setColor(QPalette::WindowText, QColor(0xfc, 0xfc, 0xfc));
    palette.setColor(QPalette::Base, QColor(0x14, 0x16, 0x18));
    palette.setColor(QPalette::Text, QColor(0xfc, 0xfc, 0xfc));
    palette.setColor(QPalette::Button, QColor(0x29, 0x2c, 0x30));
    palette.setColor(QPalette::ButtonText, QColor(0xfc, 0xfc, 0xfc));
    return palette;
}

// The application style and palette, switched until the guard goes.
class StyleScope
{
public:
    StyleScope(const QString &name, bool dark)
        : m_previous(QApplication::style()->name())
        , m_palette(QApplication::palette())
    {
        if (QStyle *style = QStyleFactory::create(name)) {
            QApplication::setStyle(style);
            QApplication::setPalette(dark ? darkPalette() : style->standardPalette());
            m_available = true;
        }
    }
    ~StyleScope()
    {
        QApplication::setStyle(m_previous);
        QApplication::setPalette(m_palette);
    }
    bool available() const
    {
        return m_available;
    }

private:
    QString m_previous;
    QPalette m_palette;
    bool m_available = false;
};
}

class AvdWorkspaceDialogTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    // Cache
    void reopeningWithinHourUsesCache();
    void reopeningAfterHourFetchesAgain();
    void refreshBypassesCache();
    void failedUpdateKeepsCardsMarkedStale();
    void partialFetchDoesNotReplaceCache();
    void emptyResultIsCached();
    void signOutForgetsCacheOfAccount();
    void signOutDropsLateDiscovery();
    void signOutElsewhereDropsLateDiscovery();
    void launchFromCacheGetsFreshToken();
    void expiredTokenIsRefreshedBeforeLaunch();
    void clockSetBackRenewsTokenBeforeLaunch();
    void launchAfterRestartFindsDesktopInFreshFeed();
    // Cards
    void separatesDevicesAndApps();
    void keyboardOpensDesktop();
    void appsAreShownButNotOpened();
    void longTitlesAreElidedWithTooltip();
    void cardsFitNarrowAndWideWindows();
    void cardsFillEachRow_data();
    void cardsFillEachRow();
    void dialogCardsFillEachRow_data();
    void dialogCardsFillEachRow();
    void showsSignedOutEmptyAndErrorStates();
    void searchFiltersCards();
    void showsServerIcon();
    void desktopShowsServerImage();
    void smallServerImageIsNotEnlarged_data();
    void smallServerImageIsNotEnlarged();
    void largeServerImageKeepsAspectRatio();
    void desktopArtIsDrawnPerScale();
    void untrustedTextIsNotMarkup();
    void controlIconsFollowPalette();

private:
    QDateTime m_now;
    std::unique_ptr<WorkspaceCache> newCache(const QString &directory = QString())
    {
        auto cache = std::make_unique<WorkspaceCache>(directory);
        cache->setClock([this]() {
            return m_now;
        });
        return cache;
    }
};

void AvdWorkspaceDialogTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QVERIFY(qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS") == QLatin1String("disabled:"));
}

void AvdWorkspaceDialogTest::init()
{
    m_now = Start;
}

void AvdWorkspaceDialogTest::reopeningWithinHourUsesCache()
{
    FakeNetwork network;
    routeAll(network);
    CountingTokenStore store;
    auto cache = newCache();
    {
        AvdWorkspaceDialog first(&network, &store, publicCloud(), nullptr, cache.get());
        first.setInteractiveSignIn(answerWithCode);
        first.signIn();
        QTRY_COMPARE(first.resourceModel()->rowCount(), 2);
    }
    const int requests = network.requests.size();
    const int reads = store.reads;

    m_now = Start.addSecs(3599);
    AvdWorkspaceDialog second(&network, &store, publicCloud(), nullptr, cache.get());
    second.setAccount(Alice);
    second.start();
    QCOMPARE(second.resourceModel()->rowCount(), 2);
    QTest::qWait(50);
    QCOMPARE(network.requests.size(), requests);
    // Showing a fresh list needs neither the keychain nor a token.
    QCOMPARE(store.reads, reads);
    QVERIFY(!second.isShowingStaleList());
    QVERIFY(second.statusText().contains(QLocale().toString(Start.toLocalTime().time(), QLocale::ShortFormat)));
}

void AvdWorkspaceDialogTest::reopeningAfterHourFetchesAgain()
{
    FakeNetwork network;
    routeAll(network);
    CountingTokenStore store;
    store.tokens.insert(Alice, QStringLiteral("stored"));
    auto cache = newCache();
    cache->store(publicCloud(), Alice, {}, {});

    m_now = Start.addSecs(3600);
    AvdWorkspaceDialog dialog(&network, &store, publicCloud(), nullptr, cache.get());
    dialog.setAccount(Alice);
    dialog.start();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 2);
    QCOMPARE(urls(network), (QList<QUrl>{publicCloud().tokenUrl(), DiscoveryUrl, FeedUrl}));
    QCOMPARE(cache->lookup(publicCloud(), Alice)->fetchedAt, m_now);
    QVERIFY(!dialog.isShowingStaleList());
}

void AvdWorkspaceDialogTest::refreshBypassesCache()
{
    FakeNetwork network;
    routeAll(network);
    CountingTokenStore store;
    store.tokens.insert(Alice, QStringLiteral("stored"));
    auto cache = newCache();
    cache->store(publicCloud(), Alice, {}, {});

    m_now = Start.addSecs(60);
    AvdWorkspaceDialog dialog(&network, &store, publicCloud(), nullptr, cache.get());
    dialog.setAccount(Alice);
    dialog.start();
    QCOMPARE(dialog.resourceModel()->rowCount(), 0);
    QVERIFY(network.requests.isEmpty());

    dialog.refresh();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 2);
    QCOMPARE(urls(network), (QList<QUrl>{publicCloud().tokenUrl(), DiscoveryUrl, FeedUrl}));
    QCOMPARE(cache->lookup(publicCloud(), Alice)->fetchedAt, Start.addSecs(60));

    // With a valid token, a second refresh goes straight to the feed.
    m_now = Start.addSecs(120);
    dialog.refresh();
    QTRY_COMPARE(network.requests.size(), 5);
    QCOMPARE(urls(network, 3), (QList<QUrl>{DiscoveryUrl, FeedUrl}));
    QTRY_COMPARE(cache->lookup(publicCloud(), Alice)->fetchedAt, Start.addSecs(120));
}

void AvdWorkspaceDialogTest::failedUpdateKeepsCardsMarkedStale()
{
    FakeNetwork network;
    routeAll(network);
    network.route(publicCloud().tokenUrl(), 500, R"({"error":"temporarily_unavailable"})");
    CountingTokenStore store;
    store.tokens.insert(Alice, QStringLiteral("stored"));
    auto cache = newCache();
    {
        FakeNetwork seed;
        routeAll(seed);
        AvdWorkspaceDialog first(&seed, &store, publicCloud(), nullptr, cache.get());
        first.setAccount(Alice);
        first.refresh();
        QTRY_COMPARE(first.resourceModel()->rowCount(), 2);
    }

    m_now = Start.addSecs(2 * 3600);
    AvdWorkspaceDialog dialog(&network, &store, publicCloud(), nullptr, cache.get());
    dialog.setAccount(Alice);
    dialog.start();
    QCOMPARE(dialog.resourceModel()->rowCount(), 2);
    QVERIFY(dialog.isShowingStaleList());
    QTRY_VERIFY(!network.requests.isEmpty());
    QTest::qWait(100);
    QCOMPARE(dialog.resourceModel()->rowCount(), 2);
    QVERIFY(dialog.isShowingStaleList());
    QVERIFY(dialog.statusText().contains(QLocale().toString(Start.toLocalTime().time(), QLocale::ShortFormat)));
    QVERIFY(dialog.placeholderText().isEmpty());
    QCOMPARE(cache->lookup(publicCloud(), Alice)->fetchedAt, Start);
}

void AvdWorkspaceDialogTest::partialFetchDoesNotReplaceCache()
{
    FakeNetwork network;
    routeAll(network);
    CountingTokenStore store;
    store.tokens.insert(Alice, QStringLiteral("stored"));
    auto cache = newCache();
    AvdWorkspaceDialog dialog(&network, &store, publicCloud(), nullptr, cache.get());
    dialog.setAccount(Alice);
    dialog.refresh();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 2);

    network.route(FeedUrl, 503, "unavailable");
    m_now = Start.addSecs(300);
    dialog.refresh();
    QTRY_COMPARE(network.requests.size(), 5);
    QTest::qWait(50);
    const auto entry = cache->lookup(publicCloud(), Alice);
    QCOMPARE(entry->resources.size(), 2);
    QCOMPARE(entry->fetchedAt, Start);

    // Nor does a failed discovery.
    network.route(DiscoveryUrl, 500, "error");
    dialog.refresh();
    QTRY_COMPARE(network.requests.size(), 6);
    QTest::qWait(50);
    QCOMPARE(cache->lookup(publicCloud(), Alice)->resources.size(), 2);
    QCOMPARE(dialog.resourceModel()->rowCount(), 2);
}

void AvdWorkspaceDialogTest::emptyResultIsCached()
{
    FakeNetwork network;
    routeAll(network, feed({}));
    CountingTokenStore store;
    store.tokens.insert(Alice, QStringLiteral("stored"));
    auto cache = newCache();
    {
        AvdWorkspaceDialog first(&network, &store, publicCloud(), nullptr, cache.get());
        first.setAccount(Alice);
        first.start();
        QTRY_VERIFY(!first.placeholderText().isEmpty() && network.requests.size() == 3);
    }
    QVERIFY(cache->lookup(publicCloud(), Alice));
    AvdWorkspaceDialog second(&network, &store, publicCloud(), nullptr, cache.get());
    second.setAccount(Alice);
    second.start();
    QTest::qWait(50);
    QCOMPARE(network.requests.size(), 3);
    QVERIFY(!second.placeholderText().isEmpty());
}

void AvdWorkspaceDialogTest::signOutForgetsCacheOfAccount()
{
    QTemporaryDir dir;
    FakeNetwork network;
    CountingTokenStore store;
    auto cache = newCache(dir.path());
    const QString bob = QStringLiteral("bob@example.org");
    cache->store(publicCloud(), Alice, {}, {});
    cache->store(publicCloud(), bob, {}, {});
    AvdWorkspaceDialog dialog(&network, &store, publicCloud(), nullptr, cache.get());
    dialog.setAccount(Alice);
    dialog.start();
    dialog.signOut();
    QVERIFY(!cache->lookup(publicCloud(), Alice));
    QVERIFY(!QFile::exists(cache->filePath(publicCloud(), Alice)));
    QVERIFY(cache->lookup(publicCloud(), bob));
    QCOMPARE(dialog.resourceModel()->rowCount(), 0);
}

void AvdWorkspaceDialogTest::signOutDropsLateDiscovery()
{
    FakeNetwork network;
    routeAll(network);
    CountingTokenStore store;
    store.tokens.insert(Alice, QStringLiteral("stored"));
    auto cache = newCache();
    AvdWorkspaceDialog dialog(&network, &store, publicCloud(), nullptr, cache.get());
    dialog.setAccount(Alice);
    dialog.start();
    // The feed is being fetched when the user signs out.
    QTRY_COMPARE(network.requests.size(), 2);
    dialog.signOut();
    QTest::qWait(100);
    QCOMPARE(dialog.resourceModel()->rowCount(), 0);
    QVERIFY(!cache->lookup(publicCloud(), Alice));
    QVERIFY(dialog.account().isEmpty());
}

void AvdWorkspaceDialogTest::signOutElsewhereDropsLateDiscovery()
{
    FakeNetwork network;
    routeAll(network);
    FakeNetwork otherNetwork;
    CountingTokenStore store;
    store.tokens.insert(Alice, QStringLiteral("stored"));
    auto cache = newCache();
    AvdWorkspaceDialog loading(&network, &store, publicCloud(), nullptr, cache.get());
    loading.setAccount(Alice);
    loading.start();
    QTRY_COMPARE(network.requests.size(), 2);

    // Another window on the same cache signs Alice out while the first is still fetching.
    AvdWorkspaceDialog other(&otherNetwork, &store, publicCloud(), nullptr, cache.get());
    other.setAccount(Alice);
    other.signOut();
    QTRY_VERIFY(network.requests.size() >= 3);
    QTest::qWait(100);
    QVERIFY(!cache->lookup(publicCloud(), Alice));
}

void AvdWorkspaceDialogTest::launchFromCacheGetsFreshToken()
{
    FakeNetwork network;
    routeAll(network);
    CountingTokenStore store;
    auto cache = newCache();
    {
        AvdWorkspaceDialog first(&network, &store, publicCloud(), nullptr, cache.get());
        first.setInteractiveSignIn(answerWithCode);
        first.signIn();
        QTRY_COMPARE(first.resourceModel()->rowCount(), 2);
    }
    network.route(publicCloud().tokenUrl(), 200, tokenJson(QStringLiteral("access-2"), QStringLiteral("refresh-2")));
    const int before = network.requests.size();

    AvdWorkspaceDialog second(&network, &store, publicCloud(), nullptr, cache.get());
    QSignalSpy launches(&second, &AvdWorkspaceDialog::launchRequested);
    second.setAccount(Alice);
    second.start();
    second.resourceView()->setCurrentIndex(rowWithTitle(second.resourceView(), QStringLiteral("Engineering Desktop")));
    second.connectSelected();
    QTRY_COMPARE(launches.count(), 1);
    QCOMPARE(urls(network, before), (QList<QUrl>{publicCloud().tokenUrl(), DesktopRdpUrl}));
    QCOMPARE(network.requests.last().request.rawHeader("Authorization"), QByteArray("Bearer access-2"));
    const QUrlQuery form(QString::fromUtf8(network.requests.at(before).body));
    QCOMPARE(form.queryItemValue(QStringLiteral("refresh_token")), QStringLiteral("refresh-1"));
    const auto launch = launches.first().at(0).value<ConnectionLaunch>();
    const auto *profile = dynamic_cast<const RdpLaunchProfile *>(launch.payload.get());
    QVERIFY(profile && profile->hasGatewayBearerToken());
}

void AvdWorkspaceDialogTest::expiredTokenIsRefreshedBeforeLaunch()
{
    FakeNetwork network;
    routeAll(network);
    CountingTokenStore store;
    auto cache = newCache();
    AvdWorkspaceDialog dialog(&network, &store, publicCloud(), nullptr, cache.get());
    QSignalSpy launches(&dialog, &AvdWorkspaceDialog::launchRequested);
    dialog.setInteractiveSignIn(answerWithCode);
    dialog.signIn();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 2);

    // Within its lifetime the token is used as it is.
    m_now = Start.addSecs(600);
    int before = network.requests.size();
    dialog.resourceView()->setCurrentIndex(rowWithTitle(dialog.resourceView(), QStringLiteral("Engineering Desktop")));
    dialog.connectSelected();
    QTRY_COMPARE(launches.count(), 1);
    QCOMPARE(urls(network, before), (QList<QUrl>{DesktopRdpUrl}));

    // The token of the listing expired an hour later.
    m_now = Start.addSecs(3600);
    network.route(publicCloud().tokenUrl(), 200, tokenJson(QStringLiteral("access-2"), QStringLiteral("refresh-2")));
    before = network.requests.size();
    dialog.connectSelected();
    QTRY_COMPARE(launches.count(), 2);
    QCOMPARE(urls(network, before), (QList<QUrl>{publicCloud().tokenUrl(), DesktopRdpUrl}));
    QCOMPARE(network.requests.last().request.rawHeader("Authorization"), QByteArray("Bearer access-2"));
}

void AvdWorkspaceDialogTest::clockSetBackRenewsTokenBeforeLaunch()
{
    FakeNetwork network;
    routeAll(network);
    CountingTokenStore store;
    auto cache = newCache();
    AvdWorkspaceDialog dialog(&network, &store, publicCloud(), nullptr, cache.get());
    QSignalSpy launches(&dialog, &AvdWorkspaceDialog::launchRequested);
    dialog.setInteractiveSignIn(answerWithCode);
    dialog.signIn();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 2);

    // The clock was set back by two hours: the wall clock no longer tells how old the token is.
    m_now = Start.addSecs(-2 * 3600);
    network.route(publicCloud().tokenUrl(), 200, tokenJson(QStringLiteral("access-2"), QStringLiteral("refresh-2")));
    const int before = network.requests.size();
    dialog.resourceView()->setCurrentIndex(rowWithTitle(dialog.resourceView(), QStringLiteral("Engineering Desktop")));
    dialog.connectSelected();
    QTRY_COMPARE(launches.count(), 1);
    QCOMPARE(urls(network, before), (QList<QUrl>{publicCloud().tokenUrl(), DesktopRdpUrl}));
    QCOMPARE(network.requests.last().request.rawHeader("Authorization"), QByteArray("Bearer access-2"));
}

void AvdWorkspaceDialogTest::launchAfterRestartFindsDesktopInFreshFeed()
{
    QTemporaryDir dir;
    FakeNetwork network;
    routeAll(network);
    CountingTokenStore store;
    {
        auto cache = newCache(dir.path());
        AvdWorkspaceDialog first(&network, &store, publicCloud(), nullptr, cache.get());
        first.setInteractiveSignIn(answerWithCode);
        first.signIn();
        QTRY_COMPARE(first.resourceModel()->rowCount(), 2);
    }
    // KRDC restarted: only the metadata on disk is left.
    m_now = Start.addSecs(300);
    auto cache = newCache(dir.path());
    const int before = network.requests.size();
    AvdWorkspaceDialog dialog(&network, &store, publicCloud(), nullptr, cache.get());
    QSignalSpy launches(&dialog, &AvdWorkspaceDialog::launchRequested);
    dialog.setAccount(Alice);
    dialog.start();
    QCOMPARE(dialog.resourceModel()->rowCount(), 2);
    QTest::qWait(50);
    QCOMPARE(network.requests.size(), before);

    dialog.resourceView()->setCurrentIndex(rowWithTitle(dialog.resourceView(), QStringLiteral("Engineering Desktop")));
    dialog.connectSelected();
    QTRY_COMPARE(launches.count(), 1);
    QCOMPARE(urls(network, before), (QList<QUrl>{publicCloud().tokenUrl(), DiscoveryUrl, FeedUrl, DesktopRdpUrl}));
    QCOMPARE(launches.first().at(0).value<ConnectionLaunch>().name, QStringLiteral("Engineering Desktop"));
}

void AvdWorkspaceDialogTest::separatesDevicesAndApps()
{
    FakeNetwork network;
    routeAll(network);
    CountingTokenStore store;
    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    dialog.setInteractiveSignIn(answerWithCode);
    dialog.signIn();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 2);

    QTabBar *tabs = dialog.categoryBar();
    QCOMPARE(tabs->count(), 2);
    QVERIFY(tabs->tabText(0).contains(QStringLiteral("Devices")));
    QVERIFY(tabs->tabText(1).contains(QStringLiteral("Apps")));
    QCOMPARE(dialog.resourceView()->model()->rowCount(), 1);
    const QModelIndex desktop = dialog.resourceView()->model()->index(0, 0);
    QCOMPARE(desktop.data().toString(), QStringLiteral("Engineering Desktop"));
    QCOMPARE(desktop.data(AvdResourceModel::SubtitleRole).toString(), QStringLiteral("Example Workspace"));
    QVERIFY(desktop.data(AvdResourceModel::LaunchableRole).toBool());
    QVERIFY(desktop.data(Qt::AccessibleTextRole).toString().contains(QStringLiteral("Engineering Desktop")));
    QVERIFY(desktop.data(Qt::AccessibleTextRole).toString().contains(QStringLiteral("Example Workspace")));

    tabs->setCurrentIndex(1);
    QCOMPARE(dialog.resourceView()->category(), AvdResourceModel::Category::Apps);
    QCOMPARE(dialog.resourceView()->model()->rowCount(), 1);
    const QModelIndex app = dialog.resourceView()->model()->index(0, 0);
    QCOMPARE(app.data().toString(), QStringLiteral("Spreadsheet"));
    QVERIFY(!app.data(AvdResourceModel::LaunchableRole).toBool());
    QVERIFY(!app.data(Qt::DecorationRole).value<QIcon>().isNull());
}

void AvdWorkspaceDialogTest::keyboardOpensDesktop()
{
    FakeNetwork network;
    routeAll(network);
    CountingTokenStore store;
    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    QSignalSpy launches(&dialog, &AvdWorkspaceDialog::launchRequested);
    dialog.setInteractiveSignIn(answerWithCode);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    dialog.signIn();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 2);

    AvdResourceView *view = dialog.resourceView();
    QVERIFY(view->focusPolicy() & Qt::TabFocus);
    view->setFocus();
    QTRY_VERIFY(view->currentIndex().isValid());
    QTest::keyClick(view, Qt::Key_Return);
    QTRY_COMPARE(launches.count(), 1);
    QCOMPARE(launches.first().at(0).value<ConnectionLaunch>().name, QStringLiteral("Engineering Desktop"));
}

void AvdWorkspaceDialogTest::appsAreShownButNotOpened()
{
    FakeNetwork network;
    routeAll(network);
    CountingTokenStore store;
    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    QSignalSpy launches(&dialog, &AvdWorkspaceDialog::launchRequested);
    dialog.setInteractiveSignIn(answerWithCode);
    dialog.resize(880, 640);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    dialog.signIn();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 2);
    dialog.categoryBar()->setCurrentIndex(1);

    AvdResourceView *view = dialog.resourceView();
    const QModelIndex app = view->model()->index(0, 0);
    QVERIFY(app.flags() & Qt::ItemIsEnabled);
    QVERIFY(app.data(Qt::ToolTipRole).toString().contains(QStringLiteral("coming soon")));
    QVERIFY(app.data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("coming soon")));

    const int before = network.requests.size();
    view->setFocus();
    view->setCurrentIndex(app);
    QTest::keyClick(view, Qt::Key_Return);
    dialog.connectSelected();
    QTest::qWait(100);
    QCOMPARE(launches.count(), 0);
    QCOMPARE(network.requests.size(), before);
    QVERIFY(dialog.statusText().contains(QStringLiteral("coming soon")));

    const QRect rect = view->visualRect(app);
    QVERIFY(rect.isValid());
    QHelpEvent help(QEvent::ToolTip, rect.center(), view->viewport()->mapToGlobal(rect.center()));
    QCoreApplication::sendEvent(view->viewport(), &help);
    QTRY_VERIFY(QToolTip::text().contains(QStringLiteral("coming soon")));
    QToolTip::hideText();
}

void AvdWorkspaceDialogTest::longTitlesAreElidedWithTooltip()
{
    FakeNetwork network;
    routeAll(network, feed({resourceXml(QStringLiteral("desktop-1"), LongTitle, "Desktop")}));
    CountingTokenStore store;
    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    dialog.setInteractiveSignIn(answerWithCode);
    dialog.resize(480, 600);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    dialog.signIn();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 1);
    AvdResourceView *view = dialog.resourceView();
    const QModelIndex desktop = view->model()->index(0, 0);
    QVERIFY(desktop.data(Qt::ToolTipRole).toString().contains(LongTitle));
    const QRect rect = view->visualRect(desktop);
    QVERIFY(rect.right() <= view->viewport()->width());
    QCOMPARE(view->horizontalScrollBar()->maximum(), 0);
    const QImage image = view->viewport()->grab().toImage();
    QVERIFY(!image.isNull());
}

void AvdWorkspaceDialogTest::cardsFitNarrowAndWideWindows()
{
    QList<QByteArray> apps;
    for (int i = 0; i < 24; ++i) {
        apps << resourceXml(QStringLiteral("app-%1").arg(i), QStringLiteral("Example App %1").arg(i), "RemoteApp");
    }
    FakeNetwork network;
    routeAll(network, feed(apps));
    CountingTokenStore store;
    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    dialog.setInteractiveSignIn(answerWithCode);
    dialog.resize(880, 640);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    dialog.signIn();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 24);
    dialog.categoryBar()->setCurrentIndex(1);
    AvdResourceView *view = dialog.resourceView();

    const auto checkFits = [view]() {
        const QAbstractItemModel *model = view->model();
        for (int row = 0; row < model->rowCount(); ++row) {
            const QRect rect = view->visualRect(model->index(row, 0));
            if (rect.left() < 0 || rect.right() > view->viewport()->width()) {
                return false;
            }
        }
        return view->horizontalScrollBar()->maximum() == 0;
    };

    QTRY_VERIFY(view->columnCount() >= 3);
    QVERIFY(checkFits());
    QVERIFY(view->verticalScrollBar()->maximum() > 0);

    dialog.resize(420, 640);
    QTRY_COMPARE(view->columnCount(), 1);
    QVERIFY(checkFits());
    QVERIFY(view->cardSize().width() >= AvdResourceView::MinAppWidth - AvdResourceView::Gap);

    // Scrolling reaches the last card.
    view->scrollTo(view->model()->index(23, 0));
    QTRY_VERIFY(view->viewport()->rect().intersects(view->visualRect(view->model()->index(23, 0))));
}

void AvdWorkspaceDialogTest::cardsFillEachRow_data()
{
    addStyleRows();
}

void AvdWorkspaceDialogTest::cardsFillEachRow()
{
    QFETCH(QString, style);
    QFETCH(bool, dark);
    const StyleScope scope(style, dark);
    if (!scope.available()) {
        QSKIP("Style not installed");
    }
    AvdResourceModel model;
    model.setResources(syntheticApps(11));
    AvdResourceFilter filter;
    filter.setSourceModel(&model);
    filter.setCategory(AvdResourceModel::Category::Apps);
    AvdResourceView view;
    view.setModel(&filter);
    view.setCategory(AvdResourceModel::Category::Apps);
    view.resize(916, 520);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_COMPARE(gridProblem(&view), QString());

    // Narrow, wide and back, with the scroll bar coming and going as rows wrap.
    for (const QSize size : {QSize(916, 520),
                             QSize(420, 520),
                             QSize(916, 520),
                             QSize(700, 200),
                             QSize(700, 900),
                             QSize(1300, 520),
                             QSize(260, 900),
                             QSize(916, 120),
                             QSize(916, 520)}) {
        view.resize(size);
        QTRY_COMPARE(gridProblem(&view), QString());
    }

    // A column appears at exactly the width that fits one more of the narrowest card, not a pixel before.
    for (int columns = 2; columns <= 5; ++columns) {
        const int width = view.width() + columns * AvdResourceView::MinAppWidth - (listWrapWidth(&view) - 1);
        view.resize(width, 520);
        QTRY_COMPARE(view.columnCount(), columns);
        QTRY_COMPARE(gridProblem(&view), QString());
        view.resize(width - 1, 520);
        QTRY_COMPARE(view.columnCount(), columns - 1);
        QTRY_COMPARE(gridProblem(&view), QString());
    }

    // Laying out again changes nothing: the grid has settled.
    const QSize card = view.cardSize();
    const QRect last = view.visualRect(filter.index(10, 0));
    for (int i = 0; i < 3; ++i) {
        view.doItemsLayout();
        QTest::qWait(20);
        QCOMPARE(view.cardSize(), card);
        QCOMPARE(view.visualRect(filter.index(10, 0)), last);
    }
}

void AvdWorkspaceDialogTest::dialogCardsFillEachRow_data()
{
    addStyleRows();
}

void AvdWorkspaceDialogTest::dialogCardsFillEachRow()
{
    QFETCH(QString, style);
    QFETCH(bool, dark);
    const StyleScope scope(style, dark);
    if (!scope.available()) {
        QSKIP("Style not installed");
    }
    QList<QByteArray> resources{resourceXml(QStringLiteral("desktop-1"), QStringLiteral("Engineering Desktop"), "Desktop")};
    for (int i = 0; i < 11; ++i) {
        resources << resourceXml(QStringLiteral("app-%1").arg(i), QStringLiteral("Example App %1").arg(i), "RemoteApp");
    }
    FakeNetwork network;
    routeAll(network, feed(resources));
    CountingTokenStore store;
    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    dialog.setInteractiveSignIn(answerWithCode);
    // About the size of a reported window that showed three columns of applications beside an empty strip.
    dialog.resize(936, 650);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    // The cards arrive while the view is hidden behind the signed-out page.
    dialog.signIn();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 12);
    AvdResourceView *view = dialog.resourceView();
    QTRY_VERIFY(view->isVisible());
    QTRY_COMPARE(gridProblem(view), QString());

    dialog.categoryBar()->setCurrentIndex(1);
    QTRY_COMPARE(gridProblem(view), QString());
    QCOMPARE(view->columnCount(), 4);
    for (const int width : {420, 1300, 936, 700, 936}) {
        dialog.resize(width, 650);
        QTRY_COMPARE(gridProblem(view), QString());
        dialog.categoryBar()->setCurrentIndex(0);
        QTRY_COMPARE(gridProblem(view), QString());
        dialog.categoryBar()->setCurrentIndex(1);
        QTRY_COMPARE(gridProblem(view), QString());
    }

    // Resized while hidden, then shown again.
    dialog.hide();
    dialog.resize(1300, 650);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QTRY_COMPARE(gridProblem(view), QString());
}

void AvdWorkspaceDialogTest::showsSignedOutEmptyAndErrorStates()
{
    FakeNetwork network;
    CountingTokenStore store;
    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    QVERIFY(dialog.placeholderText().contains(QStringLiteral("Sign in")));

    routeAll(network, feed({resourceXml(QStringLiteral("app-1"), QStringLiteral("Spreadsheet"), "RemoteApp")}));
    dialog.setInteractiveSignIn(answerWithCode);
    dialog.signIn();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 1);
    // No desktop, but an app: the apps are shown first, and the devices page says why it is empty.
    QCOMPARE(dialog.categoryBar()->currentIndex(), 1);
    QVERIFY(dialog.placeholderText().isEmpty());
    dialog.categoryBar()->setCurrentIndex(0);
    QVERIFY(dialog.placeholderText().contains(QStringLiteral("No desktops")));

    // A failure without any card to show is shown in place of the cards.
    FakeNetwork failing;
    failing.route(publicCloud().tokenUrl(), 200, tokenJson(QStringLiteral("access-1"), QStringLiteral("refresh-1")));
    failing.route(DiscoveryUrl, 500, "error");
    AvdWorkspaceDialog broken(&failing, &store, publicCloud());
    broken.setInteractiveSignIn(answerWithCode);
    broken.signIn();
    QTRY_VERIFY(broken.placeholderText().contains(QStringLiteral("500")));
}

void AvdWorkspaceDialogTest::searchFiltersCards()
{
    FakeNetwork network;
    routeAll(network,
             feed({resourceXml(QStringLiteral("app-1"), QStringLiteral("Spreadsheet"), "RemoteApp"),
                   resourceXml(QStringLiteral("app-2"), QStringLiteral("Text Editor"), "RemoteApp")}));
    CountingTokenStore store;
    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    dialog.setInteractiveSignIn(answerWithCode);
    dialog.signIn();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 2);
    dialog.categoryBar()->setCurrentIndex(1);
    QCOMPARE(dialog.resourceView()->model()->rowCount(), 2);
    dialog.searchField()->setText(QStringLiteral("edit"));
    QCOMPARE(dialog.resourceView()->model()->rowCount(), 1);
    QCOMPARE(dialog.resourceView()->model()->index(0, 0).data().toString(), QStringLiteral("Text Editor"));
    dialog.searchField()->setText(QStringLiteral("nothing like it"));
    QVERIFY(dialog.placeholderText().contains(QStringLiteral("nothing like it")));
}

void AvdWorkspaceDialogTest::showsServerIcon()
{
    QImage image(32, 32, QImage::Format_ARGB32);
    image.fill(Qt::magenta);
    QByteArray pngData;
    QBuffer buffer(&pngData);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");

    const QUrl iconUrl(QStringLiteral("https://rdweb.wvd.microsoft.com/api/arm/feeddiscovery/tenants/t1/pngs/app-1_32_0.png"));
    const QByteArray app =
        "<Resource ID=\"app-1\" Title=\"Spreadsheet\" Type=\"RemoteApp\"><Icons>"
        "<Icon32 Dimensions=\"32x32\" FileType=\"Png\" FileURL=\"/api/arm/feeddiscovery/tenants/t1/pngs/app-1_32_0.png\"/></Icons>"
        "<HostingTerminalServers><HostingTerminalServer>"
        "<ResourceFile FileExtension=\".rdp\" URL=\"https://rdweb.wvd.microsoft.com/api/arm/feed/prod/rdp/app-1.rdp\"/>"
        "</HostingTerminalServer></HostingTerminalServers></Resource>";
    FakeNetwork network;
    routeAll(network, feed({app}));
    network.route(iconUrl, 200, pngData);
    CountingTokenStore store;
    QTemporaryDir dir;
    auto cache = newCache(dir.path());
    {
        AvdWorkspaceDialog dialog(&network, &store, publicCloud(), nullptr, cache.get());
        dialog.setInteractiveSignIn(answerWithCode);
        dialog.signIn();
        QTRY_COMPARE(dialog.resourceModel()->rowCount(), 1);
        const QModelIndex index = dialog.resourceModel()->index(0, 0);
        QTRY_VERIFY(!index.data(AvdResourceModel::ServerIconRole).value<QImage>().isNull());
    }
    // The icon comes back with the list, after a restart too, without loading it again.
    auto restarted = newCache(dir.path());
    const int before = network.requests.size();
    AvdWorkspaceDialog dialog(&network, &store, publicCloud(), nullptr, restarted.get());
    dialog.setAccount(Alice);
    dialog.start();
    QCOMPARE(dialog.resourceModel()->index(0, 0).data(AvdResourceModel::ServerIconRole).value<QImage>().pixelColor(4, 4), QColor(Qt::magenta));
    QTest::qWait(50);
    QCOMPARE(network.requests.size(), before);
}

void AvdWorkspaceDialogTest::desktopShowsServerImage()
{
    Resource desktop;
    desktop.id = QStringLiteral("desktop-1");
    desktop.tenantId = QStringLiteral("t1");
    desktop.title = QStringLiteral("Engineering Desktop");
    desktop.workspaceName = QStringLiteral("Example Workspace");
    desktop.type = Resource::Type::Desktop;
    AvdResourceModel model;
    model.setResources({desktop});
    AvdResourceView view;
    view.setModel(&model);
    view.resize(420, 200);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    const QModelIndex index = model.index(0, 0);
    // Where the delegate puts the picture: inside the card's padding, 3:2, as tall as the content.
    const QRect card =
        view.visualRect(index).adjusted(AvdResourceView::Gap / 2, AvdResourceView::Gap / 2, -AvdResourceView::Gap / 2, -AvdResourceView::Gap / 2);
    const QRect content = card.adjusted(12, 12, -12, -12);
    const QRect art(content.left(), content.top(), qRound(content.height() * 1.5), content.height());

    const QImage fallback = view.viewport()->grab().toImage();
    QVERIFY(!isMagenta(fallback.pixel(art.center())));

    // A wide, large server image: shown whole, not stretched to the 3:2 frame.
    QImage wide(640, 160, QImage::Format_ARGB32);
    wide.fill(Qt::magenta);
    model.setIcon(desktop.key(), wide);
    const QImage shown = view.viewport()->grab().toImage();
    QVERIFY(shown != fallback);
    QVERIFY(isMagenta(shown.pixel(art.center())));
    QVERIFY(isMagenta(shown.pixel(art.left() + 2, art.center().y())));
    QVERIFY(!isMagenta(shown.pixel(art.center().x(), art.top() + 4)));
    QVERIFY(!isMagenta(shown.pixel(art.center().x(), art.bottom() - 4)));

    // Without one again, the drawn artwork is back.
    model.setIcon(desktop.key(), QImage());
    QCOMPARE(view.viewport()->grab().toImage(), fallback);
}

void AvdWorkspaceDialogTest::smallServerImageIsNotEnlarged_data()
{
    QTest::addColumn<QSize>("pixels");
    QTest::addColumn<qreal>("imageDpr");
    QTest::addColumn<qreal>("screenDpr");
    QTest::addColumn<QSizeF>("expected");
    QTest::newRow("32px at 1x") << QSize(32, 32) << 1.0 << 1.0 << QSizeF(32, 32);
    QTest::newRow("32px at 2x screen") << QSize(32, 32) << 1.0 << 2.0 << QSizeF(32, 32);
    QTest::newRow("32px at 1.5x screen") << QSize(32, 32) << 1.0 << 1.5 << QSizeF(32, 32);
    QTest::newRow("64px @2x image") << QSize(64, 64) << 2.0 << 1.0 << QSizeF(32, 32);
    QTest::newRow("64px at 1x") << QSize(64, 64) << 1.0 << 1.0 << QSizeF(64, 64);
    QTest::newRow("24x16 at 1.25x screen") << QSize(24, 16) << 1.0 << 1.25 << QSizeF(24, 16);
}

void AvdWorkspaceDialogTest::smallServerImageIsNotEnlarged()
{
    QFETCH(QSize, pixels);
    QFETCH(qreal, imageDpr);
    QFETCH(qreal, screenDpr);
    QFETCH(QSizeF, expected);
    QImage icon(pixels, QImage::Format_ARGB32);
    icon.fill(Qt::magenta);
    icon.setDevicePixelRatio(imageDpr);
    const PaintedCard card = paintDesktopCard(icon, screenDpr);
    const QRectF shown = magentaBounds(card.image);
    // Its own logical size, give or take the edge of a pixel, inside the frame.
    QVERIFY2(qAbs(shown.width() - expected.width()) <= 1.0 && qAbs(shown.height() - expected.height()) <= 1.0,
             qPrintable(QStringLiteral("shown %1x%2").arg(shown.width()).arg(shown.height())));
    QVERIFY(QRectF(card.art).adjusted(-0.5, -0.5, 0.5, 0.5).contains(shown));
}

void AvdWorkspaceDialogTest::largeServerImageKeepsAspectRatio()
{
    for (const QSize &pixels : {QSize(640, 160), QSize(300, 600), QSize(1920, 1080)}) {
        QImage image(pixels, QImage::Format_ARGB32);
        image.fill(Qt::magenta);
        const PaintedCard card = paintDesktopCard(image, 2.0);
        const QRectF shown = magentaBounds(card.image);
        QVERIFY(QRectF(card.art).adjusted(-0.5, -0.5, 0.5, 0.5).contains(shown));
        // Fitted: touching the frame on one axis, the original proportions on the other.
        QVERIFY(qAbs(shown.width() - card.art.width()) <= 1.0 || qAbs(shown.height() - card.art.height()) <= 1.0);
        const qreal ratio = qreal(pixels.width()) / pixels.height();
        QVERIFY2(qAbs(shown.width() / shown.height() - ratio) / ratio < 0.06, qPrintable(QStringLiteral("%1x%2").arg(shown.width()).arg(shown.height())));
    }
}

void AvdWorkspaceDialogTest::desktopArtIsDrawnPerScale()
{
    // Device pixel sizes that coincide at different scales must not share a rendering.
    const auto render = [](const QSizeF &logical, qreal dpr) {
        QImage image((logical * dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        AvdResourceDelegate::paintDesktopArt(&painter, QRectF(QPointF(0, 0), logical), false);
        return image;
    };
    const QImage small = render({102, 68}, 2.0);
    const QImage large = render({204, 136}, 1.0);
    QCOMPARE(small.size(), large.size());
    for (const QImage &image : {small, large}) {
        // Drawn across the whole frame at its own scale, not into one corner.
        const QRectF opaque = [&image]() {
            QRect bounds;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    if (qAlpha(image.pixel(x, y)) > 128) {
                        bounds |= QRect(x, y, 1, 1);
                    }
                }
            }
            return QRectF(bounds);
        }();
        QVERIFY(opaque.width() > image.width() * 0.6);
        QVERIFY(opaque.height() > image.height() * 0.8);
        QVERIFY(qAbs(opaque.center().x() - image.width() / 2.0) < image.width() * 0.05);
    }
    // The same picture at 1x and 2x, not one scaled from the other.
    const QImage one = render({102, 68}, 1.0);
    const QRgb a = small.scaled(one.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).pixel(51, 30);
    const QRgb b = one.pixel(51, 30);
    QVERIFY(qAlpha(b) > 200);
    QVERIFY(qAbs(qRed(a) - qRed(b)) < 12 && qAbs(qGreen(a) - qGreen(b)) < 12 && qAbs(qBlue(a) - qBlue(b)) < 12);
}

void AvdWorkspaceDialogTest::untrustedTextIsNotMarkup()
{
    const QString hostile = QStringLiteral("<img src=\"https://example.org/x.png\"> <b>Bold</b> & Co");
    FakeNetwork network;
    routeAll(network,
             feed({resourceXml(QStringLiteral("desktop-1"), hostile.toHtmlEscaped(), "Desktop"),
                   resourceXml(QStringLiteral("app-1"), hostile.toHtmlEscaped(), "RemoteApp")}));
    CountingTokenStore store;
    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    dialog.setInteractiveSignIn(answerWithCode);
    dialog.signIn();
    QTRY_COMPARE(dialog.resourceModel()->rowCount(), 2);

    for (int row = 0; row < 2; ++row) {
        const QModelIndex index = dialog.resourceModel()->index(row, 0);
        QCOMPARE(index.data().toString(), hostile);
        const QString tip = index.data(Qt::ToolTipRole).toString();
        QVERIFY2(!tip.contains(QStringLiteral("<img")) && !tip.contains(QStringLiteral("<b>")), qPrintable(tip));
        // What the tooltip shows is the title as it is, then the workspace on a line of its own.
        QTextDocument shown;
        if (Qt::mightBeRichText(tip)) {
            shown.setHtml(tip);
        } else {
            shown.setPlainText(tip);
        }
        const QStringList lines = shown.toPlainText().replace(QChar::LineSeparator, QLatin1Char('\n')).split(QLatin1Char('\n'));
        QVERIFY2(lines.size() >= 2, qPrintable(shown.toPlainText()));
        QCOMPARE(lines.at(0), hostile);
        QCOMPARE(lines.at(1), QStringLiteral("Example Workspace"));
    }

    // Status and placeholder repeat titles and search text; neither is read as markup.
    dialog.categoryBar()->setCurrentIndex(1);
    dialog.resourceView()->setCurrentIndex(dialog.resourceView()->model()->index(0, 0));
    dialog.connectSelected();
    QVERIFY(dialog.statusText().contains(QStringLiteral("<b>Bold</b>")));
    dialog.searchField()->setText(QStringLiteral("<b>nothing</b>"));
    QVERIFY(dialog.placeholderText().contains(QStringLiteral("<b>nothing</b>")));

    AvdWorkspaceDialog other(&network, &store, publicCloud());
    other.setAccount(QStringLiteral("<b>mallory</b>@example.org"));
    int checked = 0;
    for (const AvdWorkspaceDialog *d : {&dialog, &other}) {
        for (const QLabel *label : d->findChildren<QLabel *>()) {
            if (label->text().contains(QStringLiteral("<b>"))) {
                QCOMPARE(label->textFormat(), Qt::PlainText);
                ++checked;
            }
        }
    }
    // Status, placeholder and account line.
    QVERIFY(checked >= 3);
}

void AvdWorkspaceDialogTest::controlIconsFollowPalette()
{
    // A theme whose symbolic icons are dark, as Breeze's are, and which is not recoloured for dark palettes.
    QTemporaryDir themes;
    const QString themeDir = themes.filePath(QStringLiteral("krdc-dark-glyphs"));
    QVERIFY(QDir().mkpath(themeDir + QStringLiteral("/22")));
    {
        QFile index(themeDir + QStringLiteral("/index.theme"));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write("[Icon Theme]\nName=krdc-dark-glyphs\nDirectories=22\n\n[22]\nSize=22\nType=Fixed\n");
    }
    QImage glyph(22, 22, QImage::Format_ARGB32);
    glyph.fill(Qt::transparent);
    {
        QPainter painter(&glyph);
        painter.fillRect(QRect(4, 4, 14, 14), QColor(0x23, 0x26, 0x29));
    }
    for (const char *name : {"search", "view-refresh", "computer", "applications-all-symbolic", "network-connect"}) {
        QVERIFY(glyph.save(themeDir + QStringLiteral("/22/") + QLatin1String(name) + QStringLiteral(".png")));
    }
    const QStringList savedPaths = QIcon::themeSearchPaths();
    const QString savedTheme = QIcon::themeName();
    const auto restore = qScopeGuard([&]() {
        QIcon::setThemeSearchPaths(savedPaths);
        QIcon::setThemeName(savedTheme);
    });
    QIcon::setThemeSearchPaths({themes.path()});
    QIcon::setThemeName(QStringLiteral("krdc-dark-glyphs"));

    FakeNetwork network;
    CountingTokenStore store;
    AvdWorkspaceDialog dialog(&network, &store, publicCloud());
    QPushButton *refresh = nullptr;
    for (QPushButton *button : dialog.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Refresh")) {
            refresh = button;
        }
    }
    QVERIFY(refresh);
    QCOMPARE(dialog.searchField()->actions().size(), 1);

    const auto check = [&](bool dark) {
        const QPalette p = palette(dark);
        dialog.setPalette(p);
        QCoreApplication::processEvents();
        const QColor background = p.color(QPalette::Window);
        QVERIFY2(iconContrast(refresh->icon(), background) >= 3, qPrintable(QString::number(iconContrast(refresh->icon(), background))));
        QVERIFY(iconContrast(dialog.searchField()->actions().first()->icon(), background) >= 3);
        QVERIFY(iconContrast(dialog.categoryBar()->tabIcon(0), background) >= 3);
        QVERIFY(iconContrast(dialog.categoryBar()->tabIcon(1), background) >= 3);
    };
    check(true);
    QVERIFY(!QTest::currentTestFailed());
    // And back, as when the user switches the colour scheme.
    check(false);
    QVERIFY(!QTest::currentTestFailed());
    check(true);
}

QTEST_MAIN(AvdWorkspaceDialogTest)

#include "avdworkspacedialogtest.moc"
