/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "rdplaunchprofile.h"
#include "avd/avdendpoints.h"

#include <QFile>
#include <QStringEncoder>
#include <QTemporaryDir>
#include <QTest>

#include <freerdp/settings.h>

namespace
{
// Synthetic fixtures only.
const QByteArray PlainFile =
    "screen mode id:i:2\r\n"
    "full address:s:desktop.example.org:3390\r\n"
    "username:s:alice@example.org\r\n"
    "domain:s:EXAMPLE\r\n"
    "redirectclipboard:i:0\r\n"
    "alternate shell:s:C:\\Windows\\explorer.exe\r\n"
    "loadbalanceinfo:s:tsv://MS Terminal Services Plugin.1.Example_Pool\r\n";

const QByteArray AvdFile =
    "full address:s:hostpool.example.org\r\n"
    "gatewayhostname:s:rdgateway.wvd.microsoft.com:443\r\n"
    "gatewayusagemethod:i:1\r\n"
    "gatewaycredentialssource:i:0\r\n"
    "enablerdsaadauth:i:1\r\n"
    "targetisaadjoined:i:1\r\n"
    "resourceprovider:s:arm\r\n"
    "wvd endpoint pool:s:00000000-0000-0000-0000-000000000001\r\n"
    "remoteapplicationmode:i:0\r\n";

// Shaped like a feed-issued, signed AVD desktop file; every value is made up.
const QByteArray SignedAvdFile =
    "full address:s:00000000-0000-0000-0000-00000000f00d\r\n"
    "alternate full address:s:00000000-0000-0000-0000-00000000f00d\r\n"
    "gatewayhostname:s:afdfp-rdgateway.example.wvd.microsoft.com:443\r\n"
    "gatewayusagemethod:i:1\r\n"
    "gatewaybrokeringtype:i:1\r\n"
    "gatewaycredentialssource:i:0\r\n"
    "promptcredentialonce:i:1\r\n"
    "authentication level:i:1\r\n"
    "resourceprovider:s:arm\r\n"
    "wvd endpoint pool:s:00000000-0000-0000-0000-000000000001\r\n"
    "geo:s:EX\r\n"
    "armpath:s:/subscriptions/00000000-0000-0000-0000-000000000000/resourceGroups/example/providers/Microsoft.DesktopVirtualization/hostpools/example\r\n"
    "aadtenantid:s:00000000-0000-0000-0000-0000000000aa\r\n"
    "diagnosticserviceurl:s:https://rdweb.wvd.microsoft.com/api/arm/diagnostics\r\n"
    "hubdiscoverygeourl:s:https://rdweb.wvd.microsoft.com/api/arm/hubdiscovery/eventhub/discoverEventhubTrace\r\n"
    "loadbalanceinfo:s:tsv://MS Terminal Services Plugin.1.Example\r\n"
    "workspace id:s:00000000-0000-0000-0000-0000000000bb\r\n"
    "remoteapplicationmode:i:0\r\n"
    "remoteapplicationprogram:s:||ExampleDesktop\r\n"
    "remotedesktopname:s:Example Desktop\r\n"
    "use multimon:i:1\r\n"
    "signscope:s:Full Address,Alternate Full Address,GatewayHostname\r\n"
    "signature:s:AAAAAAAAAAAAAAAAAAAA\r\n";

QByteArray utf16le(const QByteArray &utf8)
{
    QStringEncoder encoder(QStringEncoder::Utf16LE, QStringEncoder::Flag::WriteBom);
    return encoder(QString::fromUtf8(utf8));
}

struct Settings {
    Settings()
        : settings(freerdp_settings_new(0))
    {
    }
    ~Settings()
    {
        freerdp_settings_free(settings);
    }
    rdpSettings *settings;
};

QString settingString(rdpSettings *settings, FreeRDP_Settings_Keys_String key)
{
    return QString::fromUtf8(freerdp_settings_get_string(settings, key));
}
}

class RdpLaunchProfileTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parsesUtf8();
    void parsesUtf8WithBom();
    void parsesUtf16WithBom();
    void parsesIpv6Address_data();
    void parsesIpv6Address();
    void appliesTypedSettingsAndColonValues();
    void detectsAvd();
    void detectsAvdByGatewayHost();
    void signedAvdDesktopKeepsConfiguredAuthentication();
    void appliesGatewayBearerToken();
    void bearerTokenOnlyForTrustedGateway_data();
    void bearerTokenOnlyForTrustedGateway();
    void filtersRedirectionWithAnyLineEnding_data();
    void filtersRedirectionWithAnyLineEnding();
    void refusesEmbeddedCommandLine_data();
    void refusesEmbeddedCommandLine();
    void keepsCertificateChecks();
    void displayUrlHasNoCredentials();
    void refusesOversizedFile();
    void refusesFileWithoutAddress();
    void remoteAppIsRecognisedAndNotApplied();
    void loadsLaunchFromFile();
    void refusesWorkspaceFile();
    void refusesRemoteAppFile();
    void ignoresOtherFiles();
    void readsGatewayUsageAsFreeRdpDoes();
    void workspaceLaunchIsNotAFileImport();
};

void RdpLaunchProfileTest::parsesUtf8()
{
    QString error;
    const auto profile = RdpLaunchProfile::fromContents(PlainFile, RdpLaunchProfile::Source::LocalFile, &error);
    QVERIFY2(profile, qPrintable(error));
    QCOMPARE(profile->address(), QStringLiteral("desktop.example.org"));
    QCOMPARE(profile->port(), 3390);
    QCOMPARE(profile->username(), QStringLiteral("alice@example.org"));
    QCOMPARE(profile->domain(), QStringLiteral("EXAMPLE"));
    QVERIFY(!profile->isAzureVirtualDesktop());
    QVERIFY(!profile->usesEntraAuthentication());
}

void RdpLaunchProfileTest::parsesUtf8WithBom()
{
    QString error;
    // The address is on the first line, right behind the BOM.
    const QByteArray contents = QByteArray("\xEF\xBB\xBF") + "full address:s:bom.example.org\r\n";
    const auto profile = RdpLaunchProfile::fromContents(contents, RdpLaunchProfile::Source::LocalFile, &error);
    QVERIFY2(profile, qPrintable(error));
    QCOMPARE(profile->address(), QStringLiteral("bom.example.org"));
}

void RdpLaunchProfileTest::parsesUtf16WithBom()
{
    QString error;
    const auto profile =
        RdpLaunchProfile::fromContents(utf16le(PlainFile + "username:s:\xC3\xA9lodie@example.org\r\n"), RdpLaunchProfile::Source::LocalFile, &error);
    QVERIFY2(profile, qPrintable(error));
    QCOMPARE(profile->address(), QStringLiteral("desktop.example.org"));
    QCOMPARE(profile->port(), 3390);
    QCOMPARE(profile->username(), QStringLiteral("élodie@example.org"));
}

void RdpLaunchProfileTest::parsesIpv6Address_data()
{
    QTest::addColumn<QByteArray>("address");
    QTest::addColumn<QString>("host");
    QTest::addColumn<int>("port");

    QTest::newRow("bracketed with port") << QByteArray("[2001:db8::10]:3391") << QStringLiteral("2001:db8::10") << 3391;
    QTest::newRow("bracketed") << QByteArray("[2001:db8::10]") << QStringLiteral("2001:db8::10") << -1;
    QTest::newRow("bare") << QByteArray("2001:db8::10") << QStringLiteral("2001:db8::10") << -1;
    QTest::newRow("ipv4 with port") << QByteArray("192.0.2.7:3389") << QStringLiteral("192.0.2.7") << 3389;
}

void RdpLaunchProfileTest::parsesIpv6Address()
{
    QFETCH(QByteArray, address);
    QFETCH(QString, host);
    QFETCH(int, port);

    QString error;
    const auto profile = RdpLaunchProfile::fromContents("full address:s:" + address + "\r\n", RdpLaunchProfile::Source::LocalFile, &error);
    QVERIFY2(profile, qPrintable(error));
    QCOMPARE(profile->address(), host);
    QCOMPARE(profile->port(), port);
    QCOMPARE(profile->displayUrl().host(), host);
    QCOMPARE(profile->displayUrl().port(), port);
}

void RdpLaunchProfileTest::appliesTypedSettingsAndColonValues()
{
    QString error;
    const auto profile = RdpLaunchProfile::fromContents(PlainFile, RdpLaunchProfile::Source::LocalFile, &error);
    QVERIFY2(profile, qPrintable(error));

    Settings s;
    QVERIFY2(profile->applyTo(s.settings, &error), qPrintable(error));
    QCOMPARE(settingString(s.settings, FreeRDP_ServerHostname), QStringLiteral("desktop.example.org"));
    QCOMPARE(freerdp_settings_get_uint32(s.settings, FreeRDP_ServerPort), 3390u);
    QCOMPARE(settingString(s.settings, FreeRDP_Username), QStringLiteral("alice@example.org"));
    QCOMPARE(settingString(s.settings, FreeRDP_AlternateShell), QStringLiteral("C:\\Windows\\explorer.exe"));
    QVERIFY(!freerdp_settings_get_bool(s.settings, FreeRDP_RedirectClipboard));

    const auto length = freerdp_settings_get_uint32(s.settings, FreeRDP_LoadBalanceInfoLength);
    const auto *info = static_cast<const char *>(freerdp_settings_get_pointer(s.settings, FreeRDP_LoadBalanceInfo));
    QVERIFY(info);
    QCOMPARE(QByteArray(info, length), QByteArray("tsv://MS Terminal Services Plugin.1.Example_Pool"));
}

void RdpLaunchProfileTest::detectsAvd()
{
    QString error;
    const auto profile = RdpLaunchProfile::fromContents(utf16le(AvdFile), RdpLaunchProfile::Source::AvdWorkspace, &error);
    QVERIFY2(profile, qPrintable(error));
    QVERIFY(profile->isAzureVirtualDesktop());
    QVERIFY(profile->usesEntraAuthentication());
    QVERIFY(!profile->isRemoteApp());
    QCOMPARE(profile->gatewayHost(), QStringLiteral("rdgateway.wvd.microsoft.com"));

    Settings s;
    QVERIFY2(profile->applyTo(s.settings, &error), qPrintable(error));
    QVERIFY(freerdp_settings_get_bool(s.settings, FreeRDP_AadSecurity));
    QVERIFY(freerdp_settings_get_bool(s.settings, FreeRDP_GatewayEnabled));
    QVERIFY(freerdp_settings_get_bool(s.settings, FreeRDP_GatewayArmTransport));
    QCOMPARE(settingString(s.settings, FreeRDP_GatewayHostname), QStringLiteral("rdgateway.wvd.microsoft.com"));
    // Host and gateway identities stay separate: the file names no user.
    QVERIFY(settingString(s.settings, FreeRDP_Username).isEmpty());
    QVERIFY(settingString(s.settings, FreeRDP_GatewayUsername).isEmpty());
}

void RdpLaunchProfileTest::signedAvdDesktopKeepsConfiguredAuthentication()
{
    QString error;
    const auto profile = RdpLaunchProfile::fromContents(SignedAvdFile, RdpLaunchProfile::Source::LocalFile, &error);
    QVERIFY2(profile, qPrintable(error));
    QVERIFY(profile->isAzureVirtualDesktop());
    // A program name with mode 0 is still a desktop.
    QVERIFY(!profile->isRemoteApp());
    // Neither ARM nor a tenant ID means the host signs in with Entra ID.
    QVERIFY(!profile->usesEntraAuthentication());
    QVERIFY(profile->username().isEmpty());

    Settings s;
    QVERIFY2(profile->applyTo(s.settings, &error), qPrintable(error));
    QVERIFY(freerdp_settings_get_bool(s.settings, FreeRDP_GatewayArmTransport));
    QVERIFY(!freerdp_settings_get_bool(s.settings, FreeRDP_AadSecurity));
    QVERIFY(!freerdp_settings_get_bool(s.settings, FreeRDP_RemoteApplicationMode));
    QCOMPARE(freerdp_settings_get_uint32(s.settings, FreeRDP_AuthenticationLevel), 1u);
    QVERIFY(settingString(s.settings, FreeRDP_Username).isEmpty());
    QVERIFY(settingString(s.settings, FreeRDP_GatewayUsername).isEmpty());
}

void RdpLaunchProfileTest::detectsAvdByGatewayHost()
{
    QString error;
    const auto profile =
        RdpLaunchProfile::fromContents("full address:s:hostpool.example.org\r\ngatewayhostname:s:rdgateway-r0.wvd.microsoft.com\r\ngatewayusagemethod:i:1\r\n",
                                       RdpLaunchProfile::Source::LocalFile,
                                       &error);
    QVERIFY2(profile, qPrintable(error));
    QVERIFY(profile->isAzureVirtualDesktop());
    // AAD sign-in on the host is only used if the file asks for it.
    QVERIFY(!profile->usesEntraAuthentication());

    Settings s;
    QVERIFY2(profile->applyTo(s.settings, &error), qPrintable(error));
    QVERIFY(freerdp_settings_get_bool(s.settings, FreeRDP_GatewayArmTransport));
    QVERIFY(!freerdp_settings_get_bool(s.settings, FreeRDP_AadSecurity));

    // A look-alike host is not AVD.
    const auto other = RdpLaunchProfile::fromContents("full address:s:h.example.org\r\ngatewayhostname:s:wvd.microsoft.com.example.org\r\n",
                                                      RdpLaunchProfile::Source::LocalFile,
                                                      &error);
    QVERIFY(other);
    QVERIFY(!other->isAzureVirtualDesktop());
}

void RdpLaunchProfileTest::appliesGatewayBearerToken()
{
    QString error;
    auto profile = RdpLaunchProfile::fromContents(AvdFile, RdpLaunchProfile::Source::AvdWorkspace, &error);
    QVERIFY(profile);
    QVERIFY(!profile->hasGatewayBearerToken());
    QVERIFY(profile->setGatewayBearerToken(QStringLiteral("synthetic-token"), Avd::publicCloud()));
    QVERIFY(profile->hasGatewayBearerToken());

    Settings s;
    QVERIFY(profile->applyTo(s.settings, &error));
    QCOMPARE(settingString(s.settings, FreeRDP_GatewayHttpExtAuthBearer), QStringLiteral("synthetic-token"));
    QVERIFY(!profile->contents().contains("synthetic-token"));
}

void RdpLaunchProfileTest::bearerTokenOnlyForTrustedGateway_data()
{
    QTest::addColumn<QByteArray>("gateway");
    QTest::addColumn<bool>("trusted");

    QTest::newRow("avd") << QByteArray("rdgateway.wvd.microsoft.com") << true;
    QTest::newRow("avd-443") << QByteArray("afdfp-rdgateway-r1.wvd.microsoft.com:443") << true;
    QTest::newRow("attacker") << QByteArray("gateway.attacker.example:443") << false;
    QTest::newRow("microsoft-wide") << QByteArray("gateway.microsoft.com") << false;
    QTest::newRow("look-alike") << QByteArray("rdgateway.wvd.microsoft.com.attacker.example") << false;
    QTest::newRow("other-port") << QByteArray("rdgateway.wvd.microsoft.com:8443") << false;
    QTest::newRow("address") << QByteArray("203.0.113.7") << false;
    QTest::newRow("none") << QByteArray() << false;
    // FreeRDP uses the last value; a CR alone ends a record for it.
    QTest::newRow("cr-override") << QByteArray("rdgateway.wvd.microsoft.com\rgatewayhostname:s:gateway.attacker.example") << false;
}

void RdpLaunchProfileTest::bearerTokenOnlyForTrustedGateway()
{
    QFETCH(QByteArray, gateway);
    QFETCH(bool, trusted);

    QByteArray contents = "full address:s:hostpool.example.org\r\nresourceprovider:s:arm\r\ngatewayusagemethod:i:1\r\n";
    if (!gateway.isEmpty()) {
        contents += "gatewayhostname:s:" + gateway + "\r\n";
    }
    QString error;
    auto profile = RdpLaunchProfile::fromContents(contents, RdpLaunchProfile::Source::AvdWorkspace, &error);
    QVERIFY2(profile, qPrintable(error));

    QCOMPARE(profile->setGatewayBearerToken(QStringLiteral("synthetic-token"), Avd::publicCloud()), trusted);
    QCOMPARE(profile->hasGatewayBearerToken(), trusted);

    Settings s;
    QVERIFY2(profile->applyTo(s.settings, &error), qPrintable(error));
    QCOMPARE(settingString(s.settings, FreeRDP_GatewayHttpExtAuthBearer), trusted ? QStringLiteral("synthetic-token") : QString());
}

void RdpLaunchProfileTest::filtersRedirectionWithAnyLineEnding_data()
{
    QTest::addColumn<QByteArray>("contents");

    QTest::newRow("cr-only") << QByteArray("full address:s:h.example.org\rredirectdrives:i:1\rdrivestoredirect:s:*\rdesktopwidth:i:1234\r");
    QTest::newRow("cr-inside-crlf") << QByteArray(
        "full address:s:h.example.org\r\nignored:s:x\rredirectdrives:i:1\r\nignored:s:y\rdrivestoredirect:s:*\r\ndesktopwidth:i:1234\r\n");
    QTest::newRow("lf-cr") << QByteArray("full address:s:h.example.org\n\rredirectdrives:i:1\n\rdrivestoredirect:s:*\n\rdesktopwidth:i:1234\n");
    QTest::newRow("utf16-cr") << utf16le("full address:s:h.example.org\rredirectdrives:i:1\rdrivestoredirect:s:*\rdesktopwidth:i:1234\r");
}

void RdpLaunchProfileTest::filtersRedirectionWithAnyLineEnding()
{
    QFETCH(QByteArray, contents);

    QString error;
    const auto profile = RdpLaunchProfile::fromContents(contents, RdpLaunchProfile::Source::LocalFile, &error);
    QVERIFY2(profile, qPrintable(error));
    QCOMPARE(profile->address(), QStringLiteral("h.example.org"));

    Settings s;
    QVERIFY2(profile->applyTo(s.settings, &error), qPrintable(error));
    QVERIFY(!freerdp_settings_get_bool(s.settings, FreeRDP_RedirectDrives));
    QVERIFY(settingString(s.settings, FreeRDP_DrivesToRedirect).isEmpty());
    QVERIFY(!freerdp_settings_get_bool(s.settings, FreeRDP_DeviceRedirection));
    // Other records after a CR still apply.
    QCOMPARE(freerdp_settings_get_uint32(s.settings, FreeRDP_DesktopWidth), 1234u);
}

void RdpLaunchProfileTest::refusesEmbeddedCommandLine_data()
{
    QTest::addColumn<QByteArray>("contents");

    QTest::newRow("line") << QByteArray("full address:s:h.example.org\r\n/drive:home,/\r\n");
    QTest::newRow("indented") << QByteArray("full address:s:h.example.org\r\n  /aad-helper:/tmp/synthetic-helper\r\n");
    QTest::newRow("after-cr") << QByteArray("full address:s:h.example.org\r\nignored:s:x\r/gateway:g:gateway.attacker.example\r\n");
    QTest::newRow("first") << QByteArray("/v:other.example.org\nfull address:s:h.example.org\n");
}

void RdpLaunchProfileTest::refusesEmbeddedCommandLine()
{
    QFETCH(QByteArray, contents);

    // Refused whether or not FreeRDP was built to read these lines.
    QString error;
    QVERIFY(!RdpLaunchProfile::fromContents(contents, RdpLaunchProfile::Source::LocalFile, &error));
    QVERIFY(!error.isEmpty());
}

void RdpLaunchProfileTest::keepsCertificateChecks()
{
    QString error;
    const auto profile =
        RdpLaunchProfile::fromContents("full address:s:h.example.org\r\nauthentication level:i:0\r\n", RdpLaunchProfile::Source::LocalFile, &error);
    QVERIFY(profile);

    Settings s;
    QVERIFY(profile->applyTo(s.settings, &error));
    QVERIFY(!freerdp_settings_get_bool(s.settings, FreeRDP_IgnoreCertificate));
    QVERIFY(!freerdp_settings_get_bool(s.settings, FreeRDP_AutoAcceptCertificate));
    // 0 would connect without warning when the server cannot be verified.
    QCOMPARE(freerdp_settings_get_uint32(s.settings, FreeRDP_AuthenticationLevel), 2u);
}

void RdpLaunchProfileTest::displayUrlHasNoCredentials()
{
    QString error;
    const auto profile = RdpLaunchProfile::fromContents(PlainFile + "password:s:not-in-url\r\n", RdpLaunchProfile::Source::LocalFile, &error);
    QVERIFY(profile);
    const QUrl url = profile->displayUrl();
    QCOMPARE(url.scheme(), QStringLiteral("rdp"));
    QCOMPARE(url.host(), QStringLiteral("desktop.example.org"));
    QCOMPARE(url.port(), 3390);
    QVERIFY(url.userName().isEmpty());
    QVERIFY(url.password().isEmpty());
    QVERIFY(!url.toString().contains(QStringLiteral("not-in-url")));
    QVERIFY(!url.toString().contains(QStringLiteral("alice")));
}

void RdpLaunchProfileTest::refusesOversizedFile()
{
    QString error;
    QByteArray contents = "full address:s:h.example.org\r\n";
    contents += QByteArray(RdpLaunchProfile::MaxFileSize, ' ');
    QVERIFY(!RdpLaunchProfile::fromContents(contents, RdpLaunchProfile::Source::LocalFile, &error));
    QVERIFY(!error.isEmpty());
}

void RdpLaunchProfileTest::refusesFileWithoutAddress()
{
    QString error;
    QVERIFY(!RdpLaunchProfile::fromContents("username:s:alice\r\n", RdpLaunchProfile::Source::LocalFile, &error));
    QVERIFY(!error.isEmpty());
}

void RdpLaunchProfileTest::remoteAppIsRecognisedAndNotApplied()
{
    QString error;
    const auto profile = RdpLaunchProfile::fromContents("full address:s:h.example.org\r\nremoteapplicationmode:i:1\r\nremoteapplicationprogram:s:||calc\r\n",
                                                        RdpLaunchProfile::Source::AvdWorkspace,
                                                        &error);
    QVERIFY(profile);
    QVERIFY(profile->isRemoteApp());
    Settings s;
    QVERIFY(!profile->applyTo(s.settings, &error));
    QVERIFY(!error.isEmpty());
}

void RdpLaunchProfileTest::loadsLaunchFromFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Example Desktop.rdp"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(utf16le(PlainFile));
    file.close();

    QString error;
    const ConnectionLaunch launch = rdpLaunchFromFile(QUrl::fromLocalFile(path), &error);
    QVERIFY2(launch.isValid(), qPrintable(error));
    QCOMPARE(launch.url, QUrl(QStringLiteral("rdp://desktop.example.org:3390")));
    QCOMPARE(launch.name, QStringLiteral("Example Desktop"));
    QVERIFY(!launch.remember);
    QVERIFY(launch.payload);
    QCOMPARE(launch.payload->kind(), RdpLaunchProfile::payloadKind());
    const auto *profile = dynamic_cast<const RdpLaunchProfile *>(launch.payload.get());
    QVERIFY(profile);
    QCOMPARE(profile->username(), QStringLiteral("alice@example.org"));
}

void RdpLaunchProfileTest::refusesWorkspaceFile()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("workspace.rdpw"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("full address:s:h.example.org\r\n");
    file.close();

    QString error;
    QVERIFY(!rdpLaunchFromFile(QUrl::fromLocalFile(path), &error).isValid());
    QVERIFY(error.contains(QStringLiteral(".rdpw")));
}

void RdpLaunchProfileTest::refusesRemoteAppFile()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("app.rdp"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("full address:s:h.example.org\r\nremoteapplicationmode:i:1\r\n");
    file.close();

    QString error;
    QVERIFY(!rdpLaunchFromFile(QUrl::fromLocalFile(path), &error).isValid());
    QVERIFY(!error.isEmpty());
}

void RdpLaunchProfileTest::ignoresOtherFiles()
{
    QString error;
    QVERIFY(!rdpLaunchFromFile(QUrl::fromLocalFile(QStringLiteral("/nonexistent/connection.vnc")), &error).isValid());
    QVERIFY(error.isEmpty());
}

void RdpLaunchProfileTest::readsGatewayUsageAsFreeRdpDoes()
{
    QString error;
    auto profile = RdpLaunchProfile::fromContents(AvdFile, RdpLaunchProfile::Source::LocalFile, &error);
    QVERIFY2(profile, qPrintable(error));
    QCOMPARE(profile->gatewayUsageMethod(), 1);
    QCOMPARE(profile->gatewayAddress(), QStringLiteral("rdgateway.wvd.microsoft.com:443"));

    profile = RdpLaunchProfile::fromContents("full address:s:h.example.org\r\ngatewayhostname:s:gw.example.org\r\ngatewayusagemethod:i:1x\r\n",
                                             RdpLaunchProfile::Source::LocalFile,
                                             &error);
    QVERIFY2(profile, qPrintable(error));
    QCOMPARE(profile->gatewayUsageMethod(), 1);
    Settings s;
    QVERIFY(profile->applyTo(s.settings, &error));
    QVERIFY(freerdp_settings_get_bool(s.settings, FreeRDP_GatewayEnabled));

    profile = RdpLaunchProfile::fromContents("full address:s:h.example.org\r\n", RdpLaunchProfile::Source::LocalFile, &error);
    QVERIFY2(profile, qPrintable(error));
    QVERIFY(profile->gatewayUsageMethod() != 1 && profile->gatewayUsageMethod() != 2);
}

void RdpLaunchProfileTest::workspaceLaunchIsNotAFileImport()
{
    QString error;
    auto profile = RdpLaunchProfile::fromContents(AvdFile, RdpLaunchProfile::Source::AvdWorkspace, &error);
    QVERIFY2(profile, qPrintable(error));
    const ConnectionLaunch launch = rdpLaunchFromProfile(std::make_shared<const RdpLaunchProfile>(std::move(*profile)), QStringLiteral("Desktop"));
    QVERIFY(launch.isValid());
    QVERIFY(!launch.fromFile);
    QVERIFY(launch.fileSettings.isEmpty());
}

QTEST_GUILESS_MAIN(RdpLaunchProfileTest)

#include "rdplaunchprofiletest.moc"
