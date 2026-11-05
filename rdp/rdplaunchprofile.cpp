/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "rdplaunchprofile.h"

#include "avd/avdendpoints.h"

#include <KLocalizedString>

#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QList>
#include <QStringDecoder>

#include <memory>

#include <freerdp/client/file.h>

namespace
{
struct RdpFileDeleter {
    void operator()(rdpFile *file) const
    {
        freerdp_client_rdp_file_free(file);
    }
};
using RdpFilePtr = std::unique_ptr<rdpFile, RdpFileDeleter>;

// Devices KRDC shares are chosen in its host settings, not by a file that may
// come from anywhere.
const QList<QByteArray> DeviceRedirectionKeys = {
    "drivestoredirect",
    "devicestoredirect",
    "usbdevicestoredirect",
    "camerastoredirect",
    "redirectdrives",
    "redirectcomports",
    "redirectposdevices",
};

std::optional<QByteArray> toUtf8(const QByteArray &contents, QString *errorMessage)
{
    const auto fail = [errorMessage]() -> std::optional<QByteArray> {
        if (errorMessage) {
            *errorMessage = i18nc("@info", "The connection file is not valid text.");
        }
        return std::nullopt;
    };

    QStringDecoder::Encoding encoding = QStringDecoder::Utf8;
    qsizetype skip = 0;
    if (contents.startsWith("\xFF\xFE")) {
        encoding = QStringDecoder::Utf16LE;
        skip = 2;
    } else if (contents.startsWith("\xFE\xFF")) {
        encoding = QStringDecoder::Utf16BE;
        skip = 2;
    } else if (contents.startsWith("\xEF\xBB\xBF")) {
        skip = 3;
    } else if (contents.size() >= 2 && contents.at(0) != '\0' && contents.at(1) == '\0') {
        // Windows writes UTF-16 files without a BOM in some versions.
        encoding = QStringDecoder::Utf16LE;
    }

    QStringDecoder decoder(encoding);
    QString text = decoder(QByteArrayView(contents).sliced(skip));
    if (decoder.hasError() || text.contains(QChar::Null)) {
        return fail();
    }
    // FreeRDP ends a record at any CR or LF; filtering must see the same records.
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text.toUtf8();
}

// Lines starting with '/' are FreeRDP command-line options in builds that allow them.
bool hasCommandLineRecord(const QByteArray &utf8)
{
    for (const QByteArray &line : utf8.split('\n')) {
        if (line.trimmed().startsWith('/')) {
            return true;
        }
    }
    return false;
}

QByteArray withoutKeys(const QByteArray &contents, const QList<QByteArray> &keys)
{
    QByteArray result;
    result.reserve(contents.size());
    for (const QByteArray &line : contents.split('\n')) {
        const QByteArray key = line.left(line.indexOf(':')).trimmed().toLower();
        if (!keys.contains(key)) {
            result += line + '\n';
        }
    }
    return result;
}

RdpFilePtr parse(const QByteArray &utf8)
{
    RdpFilePtr file(freerdp_client_rdp_file_new());
    if (!file) {
        return {};
    }
    if (!freerdp_client_parse_rdp_file_buffer(file.get(), reinterpret_cast<const BYTE *>(utf8.constData()), static_cast<size_t>(utf8.size()))) {
        return {};
    }
    return file;
}

QString stringOption(const rdpFile *file, const char *name)
{
    return QString::fromUtf8(freerdp_client_rdp_file_get_string_option(file, name)).trimmed();
}
}

bool rdpSplitAddress(const QString &address, QString *host, int *port)
{
    QString h = address.trimmed();
    int p = -1;

    if (h.startsWith(QLatin1Char('['))) {
        const qsizetype end = h.indexOf(QLatin1Char(']'));
        if (end < 0) {
            return false;
        }
        const QString rest = h.mid(end + 1);
        h = h.mid(1, end - 1);
        if (!rest.isEmpty()) {
            if (!rest.startsWith(QLatin1Char(':'))) {
                return false;
            }
            bool ok = false;
            p = rest.mid(1).toInt(&ok);
            if (!ok) {
                return false;
            }
        }
    } else if (h.count(QLatin1Char(':')) == 1) {
        const qsizetype colon = h.indexOf(QLatin1Char(':'));
        bool ok = false;
        p = h.mid(colon + 1).toInt(&ok);
        if (!ok) {
            return false;
        }
        h = h.left(colon);
    } else if (h.contains(QLatin1Char(':')) && QHostAddress(h).protocol() != QAbstractSocket::IPv6Protocol) {
        return false;
    }

    if (h.isEmpty() || (p != -1 && (p < 1 || p > 65535))) {
        return false;
    }
    *host = h;
    *port = p;
    return true;
}

QString RdpLaunchProfile::payloadKind()
{
    return QStringLiteral("rdp-file");
}

std::optional<RdpLaunchProfile> RdpLaunchProfile::fromContents(const QByteArray &contents, Source source, QString *errorMessage)
{
    if (contents.size() > MaxFileSize) {
        if (errorMessage) {
            *errorMessage = i18nc("@info", "The connection file is larger than %1 KiB, which is more than such files contain.", MaxFileSize / 1024);
        }
        return std::nullopt;
    }

    const auto utf8 = toUtf8(contents, errorMessage);
    if (!utf8) {
        return std::nullopt;
    }

    if (hasCommandLineRecord(*utf8)) {
        if (errorMessage) {
            *errorMessage = i18nc("@info", "The connection file contains FreeRDP command-line options, which KRDC does not accept from files.");
        }
        return std::nullopt;
    }

    const RdpFilePtr file = parse(*utf8);
    if (!file) {
        if (errorMessage) {
            *errorMessage = i18nc("@info", "The connection file could not be read.");
        }
        return std::nullopt;
    }

    RdpLaunchProfile profile;
    profile.m_source = source;
    profile.m_contents = *utf8;

    // FreeRDP connects to the alternate address when there is one.
    QString address = stringOption(file.get(), "alternate full address");
    if (address.isEmpty()) {
        address = stringOption(file.get(), "full address");
    }
    if (address.isEmpty() || !rdpSplitAddress(address, &profile.m_address, &profile.m_port)) {
        if (errorMessage) {
            *errorMessage = i18nc("@info", "The connection file does not contain a valid computer address.");
        }
        return std::nullopt;
    }
    if (profile.m_port == -1) {
        const int port = freerdp_client_rdp_file_get_integer_option(file.get(), "server port");
        if (port > 0 && port <= 65535) {
            profile.m_port = port;
        }
    }

    profile.m_username = stringOption(file.get(), "username");
    profile.m_domain = stringOption(file.get(), "domain");
    const qsizetype separator = profile.m_username.indexOf(QLatin1Char('\\'));
    if (separator > 0 && profile.m_domain.isEmpty()) {
        profile.m_domain = profile.m_username.left(separator);
        profile.m_username = profile.m_username.mid(separator + 1);
    }

    const QString gateway = stringOption(file.get(), "gatewayhostname");
    profile.m_gatewayAddress = gateway;
    profile.m_gatewayUsageMethod = freerdp_client_rdp_file_get_integer_option(file.get(), "gatewayusagemethod");
    if (!gateway.isEmpty() && !rdpSplitAddress(gateway, &profile.m_gatewayHost, &profile.m_gatewayPort)) {
        profile.m_gatewayHost.clear();
        profile.m_gatewayPort = -1;
    }

    profile.m_entraAuthentication = freerdp_client_rdp_file_get_integer_option(file.get(), "enablerdsaadauth") == 1;
    profile.m_remoteApp = freerdp_client_rdp_file_get_integer_option(file.get(), "remoteapplicationmode") == 1;
    profile.m_avd = source == Source::AvdWorkspace || stringOption(file.get(), "resourceprovider").compare(QLatin1String("arm"), Qt::CaseInsensitive) == 0
        || Avd::isAvdGatewayHost(profile.m_gatewayHost, Avd::publicCloud());

    return profile;
}

std::optional<RdpLaunchProfile> RdpLaunchProfile::fromFile(const QString &path, QString *errorMessage)
{
    const QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    if (suffix == QLatin1String("rdpw")) {
        if (errorMessage) {
            *errorMessage = i18nc("@info",
                                  "Workspace files (.rdpw) are not supported. To use an Azure Virtual Desktop workspace, open it from "
                                  "File ▸ Azure Virtual Desktop Workspaces….");
        }
        return std::nullopt;
    }
    if (suffix != QLatin1String("rdp")) {
        return std::nullopt;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return std::nullopt;
    }
    if (file.size() > MaxFileSize) {
        if (errorMessage) {
            *errorMessage = i18nc("@info", "The connection file is larger than %1 KiB, which is more than such files contain.", MaxFileSize / 1024);
        }
        return std::nullopt;
    }
    return fromContents(file.read(MaxFileSize + 1), Source::LocalFile, errorMessage);
}

QString RdpLaunchProfile::kind() const
{
    return payloadKind();
}

RdpLaunchProfile::Source RdpLaunchProfile::source() const
{
    return m_source;
}

QString RdpLaunchProfile::address() const
{
    return m_address;
}

int RdpLaunchProfile::port() const
{
    return m_port;
}

QString RdpLaunchProfile::username() const
{
    return m_username;
}

QString RdpLaunchProfile::domain() const
{
    return m_domain;
}

QString RdpLaunchProfile::gatewayHost() const
{
    return m_gatewayHost;
}

QString RdpLaunchProfile::gatewayAddress() const
{
    return m_gatewayAddress;
}

int RdpLaunchProfile::gatewayUsageMethod() const
{
    return m_gatewayUsageMethod;
}

bool RdpLaunchProfile::usesEntraAuthentication() const
{
    return m_entraAuthentication;
}

bool RdpLaunchProfile::isRemoteApp() const
{
    return m_remoteApp;
}

bool RdpLaunchProfile::isAzureVirtualDesktop() const
{
    return m_avd;
}

QUrl RdpLaunchProfile::displayUrl() const
{
    QUrl url;
    url.setScheme(QStringLiteral("rdp"));
    url.setHost(m_address);
    if (m_port > 0) {
        url.setPort(m_port);
    }
    return url;
}

QByteArray RdpLaunchProfile::contents() const
{
    return m_contents;
}

bool RdpLaunchProfile::setGatewayBearerToken(const QString &token, const Avd::CloudEndpoints &cloud)
{
    m_gatewayBearerToken.clear();
    m_gatewayBearerCloud.reset();
    if (!Avd::isTrustedAvdGateway(m_gatewayHost, m_gatewayPort, cloud)) {
        return false;
    }
    m_gatewayBearerToken = token;
    m_gatewayBearerCloud = cloud;
    return true;
}

bool RdpLaunchProfile::hasGatewayBearerToken() const
{
    return !m_gatewayBearerToken.isEmpty();
}

bool RdpLaunchProfile::applyTo(rdpSettings *settings, QString *errorMessage) const
{
    const auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };

    if (m_remoteApp) {
        return fail(i18nc("@info", "This is a remote application. KRDC can only show full remote desktops so far."));
    }

    const RdpFilePtr file = parse(withoutKeys(m_contents, DeviceRedirectionKeys));
    if (!file || !freerdp_client_populate_settings_from_rdp_file(file.get(), settings)) {
        return fail(i18nc("@info", "The settings of the connection file could not be applied."));
    }

    if (m_avd && !freerdp_settings_set_bool(settings, FreeRDP_GatewayArmTransport, TRUE)) {
        return fail(i18nc("@info", "The settings of the connection file could not be applied."));
    }
    if (hasGatewayBearerToken()) {
        // Checked again on what FreeRDP will connect to, not only on what KRDC read.
        const QString host = QString::fromUtf8(freerdp_settings_get_string(settings, FreeRDP_GatewayHostname));
        const int port = static_cast<int>(freerdp_settings_get_uint32(settings, FreeRDP_GatewayPort));
        if (!m_gatewayBearerCloud || !Avd::isTrustedAvdGateway(host, port, *m_gatewayBearerCloud)) {
            return fail(i18nc("@info", "The gateway of the connection file is not an Azure Virtual Desktop gateway."));
        }
        if (!freerdp_settings_set_string(settings, FreeRDP_GatewayHttpExtAuthBearer, m_gatewayBearerToken.toUtf8().constData())) {
            return fail(i18nc("@info", "The settings of the connection file could not be applied."));
        }
    }

    // Files can ask to connect without verifying the server; KRDC still asks.
    if (!freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, FALSE) || !freerdp_settings_set_bool(settings, FreeRDP_AutoAcceptCertificate, FALSE)) {
        return fail(i18nc("@info", "The settings of the connection file could not be applied."));
    }
    if (freerdp_settings_get_uint32(settings, FreeRDP_AuthenticationLevel) == 0 && !freerdp_settings_set_uint32(settings, FreeRDP_AuthenticationLevel, 2)) {
        return fail(i18nc("@info", "The settings of the connection file could not be applied."));
    }
    return true;
}

ConnectionLaunch rdpLaunchFromProfile(const std::shared_ptr<const RdpLaunchProfile> &profile, const QString &name)
{
    ConnectionLaunch launch;
    if (!profile) {
        return launch;
    }
    launch.url = profile->displayUrl();
    launch.name = name;
    launch.payload = profile;
    launch.remember = false;
    return launch;
}

ConnectionLaunch rdpLaunchFromFile(const QUrl &fileUrl, QString *errorMessage)
{
    if (!fileUrl.isLocalFile()) {
        return {};
    }
    const QString path = fileUrl.toLocalFile();
    auto profile = RdpLaunchProfile::fromFile(path, errorMessage);
    if (!profile) {
        return {};
    }
    if (profile->isRemoteApp()) {
        if (errorMessage) {
            *errorMessage = i18nc("@info", "“%1” opens a remote application. KRDC can only show full remote desktops so far.", QFileInfo(path).fileName());
        }
        return {};
    }
    return rdpLaunchFromProfile(std::make_shared<const RdpLaunchProfile>(std::move(*profile)), QFileInfo(path).completeBaseName());
}
