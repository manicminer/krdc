/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdworkspacecache.h"

#include "krdc_rdpcore_debug.h"

#include <KLocalizedString>

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <unistd.h>

namespace Avd
{

namespace
{
constexpr int FormatVersion = 1;
constexpr int StoredIconDimension = WorkspaceCache::StoredIconDimension;

QString typeName(Resource::Type type)
{
    switch (type) {
    case Resource::Type::Desktop:
        return QStringLiteral("Desktop");
    case Resource::Type::RemoteApp:
        return QStringLiteral("RemoteApp");
    case Resource::Type::Unknown:
        break;
    }
    return QStringLiteral("Unknown");
}

std::optional<Resource::Type> typeFromName(const QString &name)
{
    if (name == QLatin1String("Desktop")) {
        return Resource::Type::Desktop;
    }
    if (name == QLatin1String("RemoteApp")) {
        return Resource::Type::RemoteApp;
    }
    if (name == QLatin1String("Unknown")) {
        return Resource::Type::Unknown;
    }
    return std::nullopt;
}

QByteArray encodePng(const QImage &image)
{
    QImage icon = image;
    if (icon.width() > StoredIconDimension || icon.height() > StoredIconDimension) {
        icon = icon.scaled(StoredIconDimension, StoredIconDimension, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    icon.save(&buffer, "PNG");
    return data;
}

quint32 readBigEndian32(const QByteArray &data, qsizetype at)
{
    return (quint32(quint8(data[at])) << 24) | (quint32(quint8(data[at + 1])) << 16) | (quint32(quint8(data[at + 2])) << 8) | quint32(quint8(data[at + 3]));
}

// ICO directory entries say at most 256 pixels, but an entry may be a PNG of any size: check those headers too.
bool icoEntriesFit(const QByteArray &data, int maxDimension)
{
    if (data.size() < 6) {
        return false;
    }
    const int count = quint8(data[4]) | (quint8(data[5]) << 8);
    if (count == 0 || 6 + qsizetype(count) * 16 > data.size()) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        const qsizetype entry = 6 + qsizetype(i) * 16;
        const int width = quint8(data[entry]) ? quint8(data[entry]) : 256;
        const int height = quint8(data[entry + 1]) ? quint8(data[entry + 1]) : 256;
        if (width > maxDimension || height > maxDimension) {
            return false;
        }
        const quint32 offset =
            quint8(data[entry + 12]) | (quint8(data[entry + 13]) << 8) | (quint8(data[entry + 14]) << 16) | (quint32(quint8(data[entry + 15])) << 24);
        if (qsizetype(offset) + 24 <= data.size() && data.mid(offset, 8) == QByteArray("\x89PNG\r\n\x1a\n")) {
            if (readBigEndian32(data, offset + 16) > quint32(maxDimension) || readBigEndian32(data, offset + 20) > quint32(maxDimension)) {
                return false;
            }
        }
    }
    return true;
}

constexpr QFileDevice::Permissions OthersAccess =
    QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
}

WorkspaceCache::WorkspaceCache(const QString &directory)
    : m_directory(directory)
{
}

void WorkspaceCache::setClock(Clock clock)
{
    m_clock = std::move(clock);
}

QDateTime WorkspaceCache::now() const
{
    return m_clock ? m_clock() : QDateTime::currentDateTimeUtc();
}

QString WorkspaceCache::key(const CloudEndpoints &cloud, const QString &account)
{
    return cloud.name + QLatin1Char('\n') + cloud.authorityHost.toLower() + QLatin1Char('\n') + cloud.feedDiscoveryUrl.toString() + QLatin1Char('\n')
        + account.toLower();
}

QString WorkspaceCache::filePath(const CloudEndpoints &cloud, const QString &account) const
{
    if (m_directory.isEmpty()) {
        return {};
    }
    const QByteArray hash = QCryptographicHash::hash(key(cloud, account).toUtf8(), QCryptographicHash::Sha256).toHex();
    return m_directory + QLatin1Char('/') + QString::fromLatin1(hash) + QStringLiteral(".json");
}

std::optional<WorkspaceCache::Entry> WorkspaceCache::lookup(const CloudEndpoints &cloud, const QString &account)
{
    if (account.isEmpty()) {
        return std::nullopt;
    }
    const QString k = key(cloud, account);
    if (const auto it = m_entries.constFind(k); it != m_entries.cend()) {
        return *it;
    }
    if (m_directory.isEmpty()) {
        return std::nullopt;
    }
    auto entry = load(filePath(cloud, account));
    if (entry) {
        m_entries.insert(k, *entry);
    }
    return entry;
}

bool WorkspaceCache::isFresh(const Entry &entry) const
{
    const QDateTime current = now();
    // Not sliding, and an entry "from the future" means the clock was set back: its age is unknown.
    return entry.fetchedAt.isValid() && entry.fetchedAt <= current && entry.fetchedAt.secsTo(current) < MaxAge.count();
}

quint64 WorkspaceCache::generation(const CloudEndpoints &cloud, const QString &account) const
{
    return m_generations.value(key(cloud, account));
}

bool WorkspaceCache::store(const CloudEndpoints &cloud,
                           const QString &account,
                           const QList<Resource> &resources,
                           const QStringList &warnings,
                           std::optional<quint64> generation)
{
    if (account.isEmpty()) {
        return false;
    }
    const QString k = key(cloud, account);
    if (generation && *generation != m_generations.value(k)) {
        return false;
    }
    Entry entry;
    entry.resources = resources;
    entry.warnings = warnings;
    entry.fetchedAt = now();
    entry.hasAddresses = true;
    if (const auto previous = lookup(cloud, account)) {
        for (const Resource &resource : resources) {
            const QImage icon = previous->icons.value(resource.key());
            if (!icon.isNull()) {
                entry.icons.insert(resource.key(), icon);
            }
        }
    }
    m_entries.insert(k, entry);
    save(filePath(cloud, account), entry);
    return true;
}

void WorkspaceCache::storeIcon(const CloudEndpoints &cloud,
                               const QString &account,
                               const QString &resourceKey,
                               const QImage &icon,
                               std::optional<quint64> generation)
{
    const QString k = key(cloud, account);
    const auto it = m_entries.find(k);
    if (it == m_entries.end() || icon.isNull() || (generation && *generation != m_generations.value(k))) {
        return;
    }
    const bool listed = std::any_of(it->resources.cbegin(), it->resources.cend(), [&resourceKey](const Resource &resource) {
        return resource.key() == resourceKey;
    });
    if (!listed) {
        return;
    }
    it->icons.insert(resourceKey, icon);
    save(filePath(cloud, account), *it);
}

void WorkspaceCache::remove(const CloudEndpoints &cloud, const QString &account)
{
    const QString k = key(cloud, account);
    m_entries.remove(k);
    ++m_generations[k];
    const QString path = filePath(cloud, account);
    if (!path.isEmpty()) {
        QFile::remove(path);
    }
}

void WorkspaceCache::save(const QString &path, const Entry &entry) const
{
    if (path.isEmpty()) {
        return;
    }
    if (!QDir().mkpath(m_directory) || !QFile::setPermissions(m_directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        qCWarning(KRDC_RDP) << "Could not create the private workspace cache directory";
        return;
    }

    // Only what the cards show. Addresses are left out: they may carry signatures.
    QJsonArray resources;
    QJsonObject icons;
    qsizetype iconBytes = 0;
    for (const Resource &resource : entry.resources) {
        resources.append(QJsonObject{
            {QStringLiteral("id"), resource.id.left(MaxTextLength)},
            {QStringLiteral("title"), resource.title.left(MaxTextLength)},
            {QStringLiteral("type"), typeName(resource.type)},
            {QStringLiteral("workspace"), resource.workspaceName.left(MaxTextLength)},
            {QStringLiteral("tenantId"), resource.tenantId.left(MaxTextLength)},
            {QStringLiteral("tenantName"), resource.tenantName.left(MaxTextLength)},
        });
        const QImage icon = entry.icons.value(resource.key());
        if (!icon.isNull() && icons.size() < MaxStoredIcons) {
            const QByteArray png = encodePng(icon);
            const qsizetype decoded = qsizetype(std::min(icon.width(), StoredIconDimension)) * std::min(icon.height(), StoredIconDimension) * 4;
            if (png.size() <= MaxStoredIconSize && iconBytes + decoded <= MaxStoredIconBytes) {
                icons.insert(resource.key(), QString::fromLatin1(png.toBase64()));
                iconBytes += decoded;
            }
        }
        if (resources.size() >= MaxResources) {
            break;
        }
    }
    const QJsonObject root{
        {QStringLiteral("version"), FormatVersion},
        {QStringLiteral("fetchedAt"), entry.fetchedAt.toMSecsSinceEpoch()},
        {QStringLiteral("resources"), resources},
        {QStringLiteral("icons"), icons},
    };
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (json.size() > MaxFileSize) {
        QFile::remove(path);
        return;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
        qCWarning(KRDC_RDP) << "Could not write the workspace cache:" << file.errorString();
        return;
    }
    file.write(json);
    if (!file.commit()) {
        qCWarning(KRDC_RDP) << "Could not write the workspace cache:" << file.errorString();
    }
}

std::optional<WorkspaceCache::Entry> WorkspaceCache::load(const QString &path) const
{
    const QFileInfo info(path);
    if (!info.exists()) {
        return std::nullopt;
    }
    const auto refuse = [&path](const char *reason) -> std::optional<Entry> {
        qCWarning(KRDC_RDP) << "Ignoring the workspace cache:" << reason;
        QFile::remove(path);
        return std::nullopt;
    };
    if (!info.isFile() || info.isSymLink() || info.ownerId() != ::getuid() || (info.permissions() & OthersAccess)) {
        return refuse("not a private file");
    }
    if (info.size() > MaxFileSize) {
        return refuse("too large");
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray json = file.read(MaxFileSize + 1);
    if (json.size() > MaxFileSize) {
        return refuse("too large");
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return refuse("malformed");
    }
    const QJsonObject root = document.object();
    if (root.value(QLatin1String("version")).toInt() != FormatVersion) {
        return refuse("unknown version");
    }
    Entry entry;
    entry.hasAddresses = false;
    entry.fetchedAt = QDateTime::fromMSecsSinceEpoch(qint64(root.value(QLatin1String("fetchedAt")).toDouble(-1)), QTimeZone::UTC);
    if (root.value(QLatin1String("fetchedAt")).toDouble(-1) < 0 || !entry.fetchedAt.isValid()) {
        return refuse("no fetch time");
    }
    const QJsonArray resources = root.value(QLatin1String("resources")).toArray();
    if (resources.size() > MaxResources) {
        return refuse("too many resources");
    }
    for (const QJsonValue &value : resources) {
        const QJsonObject object = value.toObject();
        Resource resource;
        resource.id = object.value(QLatin1String("id")).toString();
        resource.title = object.value(QLatin1String("title")).toString();
        resource.workspaceName = object.value(QLatin1String("workspace")).toString();
        resource.tenantId = object.value(QLatin1String("tenantId")).toString();
        resource.tenantName = object.value(QLatin1String("tenantName")).toString();
        const auto type = typeFromName(object.value(QLatin1String("type")).toString());
        if (!type || resource.id.isEmpty()) {
            return refuse("malformed resource");
        }
        resource.type = *type;
        for (const QString *text : {&resource.id, &resource.title, &resource.workspaceName, &resource.tenantId, &resource.tenantName}) {
            if (text->size() > MaxTextLength) {
                return refuse("text too long");
            }
        }
        entry.resources << resource;
    }
    // A small file can still hold many highly compressed images: only icons of listed resources, as
    // small as KRDC writes them, and only so many, are decoded.
    const QJsonObject icons = root.value(QLatin1String("icons")).toObject();
    qsizetype iconBytes = 0;
    for (const Resource &resource : std::as_const(entry.resources)) {
        if (entry.icons.size() >= MaxStoredIcons) {
            break;
        }
        const QByteArray encoded = icons.value(resource.key()).toString().toLatin1();
        if (encoded.isEmpty() || encoded.size() > MaxStoredIconSize * 4 / 3 + 4 || entry.icons.contains(resource.key())) {
            continue;
        }
        if (const auto icon = decodeIcon(QByteArray::fromBase64(encoded), nullptr, StoredIconDimension)) {
            if (iconBytes + icon->sizeInBytes() > MaxStoredIconBytes) {
                break;
            }
            iconBytes += icon->sizeInBytes();
            entry.icons.insert(resource.key(), *icon);
        }
    }
    return entry;
}

std::optional<QImage> decodeIcon(const QByteArray &data, QString *errorMessage, int maxDimension)
{
    const auto fail = [errorMessage](const QString &message) -> std::optional<QImage> {
        if (errorMessage) {
            *errorMessage = message;
        }
        return std::nullopt;
    };
    if (data.isEmpty() || data.size() > MaxIconFileSize) {
        return fail(i18nc("@info", "The icon is empty or larger than KRDC accepts."));
    }
    // Decided from the content, not from what the server claims; only PNG and ICO.
    QByteArray format;
    if (data.startsWith("\x89PNG\r\n\x1a\n")) {
        format = "png";
    } else if (data.startsWith(QByteArray("\x00\x00\x01\x00", 4))) {
        if (!icoEntriesFit(data, maxDimension)) {
            return fail(i18nc("@info", "The icon is larger than %1 pixels a side.", maxDimension));
        }
        format = "ico";
    } else {
        return fail(i18nc("@info", "The icon is neither a PNG nor an ICO image."));
    }
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, format);
    reader.setDecideFormatFromContent(false);
    // No QImageReader::setAllocationLimit(): it is process-wide. The header's size bounds the allocation instead.
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > maxDimension || size.height() > maxDimension) {
        return fail(i18nc("@info", "The icon is larger than %1 pixels a side.", maxDimension));
    }
    QImage image;
    if (!reader.read(&image) || image.isNull() || image.width() > maxDimension || image.height() > maxDimension) {
        return fail(i18nc("@info", "The icon could not be read."));
    }
    return image;
}

} // namespace Avd
