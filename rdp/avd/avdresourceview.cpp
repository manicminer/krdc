/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdresourceview.h"

#include <KLocalizedString>

#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyleOption>

namespace
{
constexpr int DeviceCardHeight = 104;
constexpr int AppCardHeight = 68;
constexpr int Radius = 8;
constexpr int Padding = 12;

QColor mix(const QColor &a, const QColor &b, qreal amount)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * amount,
                            a.greenF() + (b.greenF() - a.greenF()) * amount,
                            a.blueF() + (b.blueF() - a.blueF()) * amount);
}

bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

qreal deviceRatio(const QPainter *painter)
{
    return painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
}

qreal snap(qreal value, qreal dpr)
{
    return std::round(value * dpr) / dpr;
}

// Centred in bounds, scaled down to fit but never up past its own logical size.
QRectF imageTarget(const QImage &image, const QRectF &bounds, qreal dpr)
{
    const QSizeF natural = image.deviceIndependentSize();
    QSizeF size = natural;
    if (size.width() > bounds.width() || size.height() > bounds.height()) {
        size = natural.scaled(bounds.size(), Qt::KeepAspectRatio);
    }
    QRectF target(QPointF(0, 0), size);
    target.moveCenter(bounds.center());
    target.moveTopLeft(QPointF(snap(target.left(), dpr), snap(target.top(), dpr)));
    return target;
}

void drawImageIn(QPainter *painter, const QImage &image, const QRectF &bounds)
{
    const QRectF target = imageTarget(image, bounds, deviceRatio(painter));
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(target, image);
    painter->restore();
}

QString subtitleOf(const Avd::Resource &resource)
{
    return resource.workspaceName.isEmpty() ? resource.tenantName : resource.workspaceName;
}
}

AvdResourceModel::AvdResourceModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void AvdResourceModel::setResources(const QList<Avd::Resource> &resources, const QHash<QString, QImage> &icons)
{
    beginResetModel();
    m_resources = resources;
    m_icons = icons;
    endResetModel();
}

void AvdResourceModel::setIcon(const QString &key, const QImage &icon)
{
    for (int row = 0; row < m_resources.size(); ++row) {
        if (m_resources.at(row).key() == key) {
            m_icons.insert(key, icon);
            Q_EMIT dataChanged(index(row), index(row), {Qt::DecorationRole, ServerIconRole});
        }
    }
}

void AvdResourceModel::clear()
{
    setResources({});
}

QList<Avd::Resource> AvdResourceModel::resources() const
{
    return m_resources;
}

std::optional<Avd::Resource> AvdResourceModel::resource(const QString &key) const
{
    for (const Avd::Resource &resource : m_resources) {
        if (resource.key() == key) {
            return resource;
        }
    }
    return std::nullopt;
}

int AvdResourceModel::count(Category category) const
{
    return std::count_if(m_resources.cbegin(), m_resources.cend(), [category](const Avd::Resource &resource) {
        return categoryOf(resource) == category;
    });
}

int AvdResourceModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_resources.size();
}

AvdResourceModel::Category AvdResourceModel::categoryOf(const Avd::Resource &resource)
{
    return resource.type == Avd::Resource::Type::RemoteApp ? Category::Apps : Category::Devices;
}

bool AvdResourceModel::canOpen(const Avd::Resource &resource)
{
    return resource.type == Avd::Resource::Type::Desktop;
}

QIcon AvdResourceModel::themeIconFor(const QString &title)
{
    // Freedesktop names, so any icon theme can answer; the feed's own icon wins when there is one.
    static const QList<std::pair<QStringList, QString>> mapping = {
        {{QStringLiteral("calculator")}, QStringLiteral("accessories-calculator")},
        {{QStringLiteral("excel"), QStringLiteral("spreadsheet"), QStringLiteral("calc")}, QStringLiteral("x-office-spreadsheet")},
        {{QStringLiteral("word"), QStringLiteral("writer"), QStringLiteral("document")}, QStringLiteral("x-office-document")},
        {{QStringLiteral("powerpoint"), QStringLiteral("impress"), QStringLiteral("presentation")}, QStringLiteral("x-office-presentation")},
        {{QStringLiteral("outlook"), QStringLiteral("mail")}, QStringLiteral("internet-mail")},
        {{QStringLiteral("edge"), QStringLiteral("chrome"), QStringLiteral("firefox"), QStringLiteral("browser")}, QStringLiteral("internet-web-browser")},
        {{QStringLiteral("explorer"), QStringLiteral("files")}, QStringLiteral("system-file-manager")},
        {{QStringLiteral("onedrive"), QStringLiteral("sharepoint")}, QStringLiteral("folder-cloud")},
        {{QStringLiteral("teams"), QStringLiteral("skype"), QStringLiteral("chat")}, QStringLiteral("internet-group-chat")},
        {{QStringLiteral("terminal"), QStringLiteral("powershell"), QStringLiteral("command prompt"), QStringLiteral("cmd")},
         QStringLiteral("utilities-terminal")},
        {{QStringLiteral("notepad"), QStringLiteral("onenote"), QStringLiteral("editor"), QStringLiteral("text")}, QStringLiteral("accessories-text-editor")},
        {{QStringLiteral("paint"), QStringLiteral("photo"), QStringLiteral("image")}, QStringLiteral("applications-graphics")},
        {{QStringLiteral("visual studio"), QStringLiteral("studio"), QStringLiteral("sql"), QStringLiteral("code")},
         QStringLiteral("applications-development")},
        {{QStringLiteral("access"), QStringLiteral("database")}, QStringLiteral("x-office-database")},
    };
    const QString lower = title.toLower();
    for (const auto &[words, name] : mapping) {
        for (const QString &word : words) {
            if (lower.contains(word) && QIcon::hasThemeIcon(name)) {
                return QIcon::fromTheme(name);
            }
        }
    }
    return QIcon::fromTheme(QStringLiteral("application-x-executable"));
}

QVariant AvdResourceModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) {
        return {};
    }
    const Avd::Resource &resource = m_resources.at(index.row());
    const bool openable = canOpen(resource);
    const QString subtitle = subtitleOf(resource);
    switch (role) {
    case Qt::DisplayRole:
        return resource.title;
    case SubtitleRole:
        return subtitle;
    case CategoryRole:
        return QVariant::fromValue(categoryOf(resource));
    case LaunchableRole:
        return openable;
    case KeyRole:
        return resource.key();
    case ServerIconRole:
        return m_icons.value(resource.key());
    case Qt::DecorationRole: {
        const QImage icon = m_icons.value(resource.key());
        if (!icon.isNull()) {
            return QIcon(QPixmap::fromImage(icon));
        }
        if (categoryOf(resource) == Category::Apps) {
            return themeIconFor(resource.title);
        }
        return QIcon::fromTheme(QStringLiteral("computer"));
    }
    case Qt::ToolTipRole: {
        // Tooltips guess whether text is markup; titles from the feed are escaped so they never are.
        QStringList lines{resource.title};
        if (!subtitle.isEmpty()) {
            lines << subtitle;
        }
        if (resource.type == Avd::Resource::Type::RemoteApp) {
            lines << i18nc("@info:tooltip", "KRDC cannot open remote applications yet; support is coming soon.");
        } else if (!openable) {
            lines << i18nc("@info:tooltip", "KRDC cannot open this kind of resource.");
        }
        for (QString &line : lines) {
            line = line.toHtmlEscaped();
        }
        return QString(QStringLiteral("<qt>") + lines.join(QStringLiteral("<br/>")) + QStringLiteral("</qt>"));
    }
    case Qt::AccessibleTextRole:
        return subtitle.isEmpty() ? resource.title : i18nc("@info accessible name: resource title, workspace", "%1, %2", resource.title, subtitle);
    case Qt::AccessibleDescriptionRole:
        if (openable) {
            return i18nc("@info accessible description", "Remote desktop. Press Enter to connect.");
        }
        return resource.type == Avd::Resource::Type::RemoteApp ? i18nc("@info accessible description", "Remote application, coming soon; cannot be opened yet.")
                                                               : i18nc("@info accessible description", "Cannot be opened.");
    }
    return {};
}

AvdResourceFilter::AvdResourceFilter(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setSortCaseSensitivity(Qt::CaseInsensitive);
    setSortLocaleAware(true);
    sort(0);
}

void AvdResourceFilter::setCategory(AvdResourceModel::Category category)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    m_category = category;
    endFilterChange(Direction::Rows);
#else
    m_category = category;
    invalidateRowsFilter();
#endif
}

AvdResourceModel::Category AvdResourceFilter::category() const
{
    return m_category;
}

void AvdResourceFilter::setSearchText(const QString &text)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    m_search = text.trimmed();
    endFilterChange(Direction::Rows);
#else
    m_search = text.trimmed();
    invalidateRowsFilter();
#endif
}

bool AvdResourceFilter::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    if (index.data(AvdResourceModel::CategoryRole).value<AvdResourceModel::Category>() != m_category) {
        return false;
    }
    return m_search.isEmpty() || index.data().toString().contains(m_search, Qt::CaseInsensitive)
        || index.data(AvdResourceModel::SubtitleRole).toString().contains(m_search, Qt::CaseInsensitive);
}

AvdResourceDelegate::AvdResourceDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

QSize AvdResourceDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (const auto *view = qobject_cast<const AvdResourceView *>(option.widget)) {
        return view->cardSize();
    }
    const bool device = index.data(AvdResourceModel::CategoryRole).value<AvdResourceModel::Category>() == AvdResourceModel::Category::Devices;
    return {device ? AvdResourceView::MinDeviceWidth : AvdResourceView::MinAppWidth, device ? DeviceCardHeight : AppCardHeight};
}

namespace
{
// The monitor's outline in a 3:2 box of height u, centred in rect.
QRectF monitorBody(const QRectF &rect, qreal dpr)
{
    const qreal u = std::min(rect.width() / 1.5, rect.height());
    const qreal top = rect.center().y() - u / 2;
    const qreal width = u * 1.16;
    const qreal height = u * 0.72;
    const qreal left = snap(rect.center().x() - width / 2, dpr);
    return QRectF(left, snap(top + u * 0.02, dpr), snap(width, dpr), snap(height, dpr));
}

qreal bezelOf(const QRectF &body)
{
    return std::max(2.5, body.height() * 0.07);
}
}

QRectF AvdResourceDelegate::desktopScreenRect(const QRectF &rect)
{
    const QRectF body = monitorBody(rect, 1.0);
    const qreal bezel = bezelOf(body);
    return body.adjusted(bezel, bezel, -bezel, -bezel);
}

void AvdResourceDelegate::paintDesktopArt(QPainter *painter, const QRectF &rect, bool dark, const QImage &screenImage)
{
    // Vectors, straight onto the target: sharp at any scale, nothing cached at the wrong one.
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    const qreal dpr = deviceRatio(painter);
    const qreal u = std::min(rect.width() / 1.5, rect.height());
    const QRectF body = monitorBody(rect, dpr);
    const qreal bezel = bezelOf(body);
    const qreal cx = body.center().x();

    // Stand: a soft shadow, a foot and a tapered neck.
    const qreal footTop = snap(body.bottom() + u * 0.13, dpr);
    const QRectF foot(snap(cx - u * 0.22, dpr), footTop, snap(u * 0.44, dpr), std::max(2.0, snap(u * 0.06, dpr)));
    QRadialGradient shadow(QPointF(cx, foot.bottom()), u * 0.34);
    shadow.setColorAt(0, QColor(0, 0, 0, dark ? 90 : 45));
    shadow.setColorAt(1, QColor(0, 0, 0, 0));
    painter->save();
    painter->translate(cx, foot.bottom());
    painter->scale(1, 0.12);
    painter->translate(-cx, -foot.bottom());
    painter->setBrush(shadow);
    painter->drawEllipse(QPointF(cx, foot.bottom()), u * 0.34, u * 0.34);
    painter->restore();

    QPolygonF neck({QPointF(cx - u * 0.07, body.bottom() - 1),
                    QPointF(cx + u * 0.07, body.bottom() - 1),
                    QPointF(cx + u * 0.10, foot.top() + 0.5),
                    QPointF(cx - u * 0.10, foot.top() + 0.5)});
    QLinearGradient metal(QPointF(cx - u * 0.10, 0), QPointF(cx + u * 0.10, 0));
    metal.setColorAt(0, dark ? QColor(0x4e, 0x54, 0x5a) : QColor(0x8d, 0x95, 0x9c));
    metal.setColorAt(0.5, dark ? QColor(0x6b, 0x72, 0x79) : QColor(0xb4, 0xbb, 0xc1));
    metal.setColorAt(1, dark ? QColor(0x4e, 0x54, 0x5a) : QColor(0x8d, 0x95, 0x9c));
    painter->setBrush(metal);
    painter->drawPolygon(neck);
    QLinearGradient footFill(foot.topLeft(), foot.bottomLeft());
    footFill.setColorAt(0, dark ? QColor(0x7a, 0x81, 0x88) : QColor(0xc4, 0xca, 0xcf));
    footFill.setColorAt(1, dark ? QColor(0x4a, 0x50, 0x56) : QColor(0x8a, 0x92, 0x99));
    painter->setBrush(footFill);
    painter->drawRoundedRect(foot, foot.height() / 2, foot.height() / 2);

    // Body, with a faint rim of light along the top edge.
    const qreal radius = std::max(3.0, u * 0.07);
    QLinearGradient shell(body.topLeft(), body.bottomLeft());
    shell.setColorAt(0, dark ? QColor(0x5d, 0x63, 0x6a) : QColor(0x4b, 0x52, 0x59));
    shell.setColorAt(1, dark ? QColor(0x3b, 0x40, 0x46) : QColor(0x2a, 0x2f, 0x34));
    painter->setBrush(shell);
    painter->drawRoundedRect(body, radius, radius);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(QColor(255, 255, 255, dark ? 50 : 60), 1.0 / dpr));
    painter->drawRoundedRect(body.adjusted(0.5 / dpr, 0.5 / dpr, -0.5 / dpr, -0.5 / dpr), radius, radius);
    painter->setPen(Qt::NoPen);

    // Screen: Breeze blue sliding into teal, a sheen, a gentle wave and a taskbar.
    const QRectF screen = body.adjusted(bezel, bezel, -bezel, -bezel);
    const qreal screenRadius = std::max(1.5, radius - bezel * 0.6);
    QPainterPath screenPath;
    screenPath.addRoundedRect(screen, screenRadius, screenRadius);
    painter->save();
    painter->setClipPath(screenPath);
    QLinearGradient glass(screen.topLeft(), screen.bottomRight());
    glass.setColorAt(0, dark ? QColor(0x2f, 0x8f, 0xcf) : QColor(0x5e, 0xbd, 0xf2));
    glass.setColorAt(0.55, dark ? QColor(0x1d, 0x6c, 0xa8) : QColor(0x2a, 0x93, 0xdb));
    glass.setColorAt(1, dark ? QColor(0x15, 0x5d, 0x6c) : QColor(0x1d, 0x8f, 0x95));
    painter->fillRect(screen, glass);

    const qreal sw = screen.width();
    const qreal sh = screen.height();
    const auto at = [&screen, sw, sh](qreal x, qreal y) {
        return QPointF(screen.left() + sw * x, screen.top() + sh * y);
    };
    const qreal waveAlpha = screenImage.isNull() ? 1.0 : 0.5;
    QPainterPath wave(at(0, 0.70));
    wave.cubicTo(at(0.30, 0.48), at(0.62, 0.92), at(1, 0.56));
    wave.lineTo(at(1, 1));
    wave.lineTo(at(0, 1));
    wave.closeSubpath();
    painter->fillPath(wave, QColor(0x3d, 0xd9, 0xc1, int((dark ? 70 : 90) * waveAlpha)));
    QPainterPath crest(at(0, 0.80));
    crest.cubicTo(at(0.35, 0.62), at(0.70, 0.98), at(1, 0.74));
    crest.lineTo(at(1, 1));
    crest.lineTo(at(0, 1));
    crest.closeSubpath();
    painter->fillPath(crest, QColor(255, 255, 255, int((dark ? 26 : 40) * waveAlpha)));
    QPainterPath sheen(at(0, 0));
    sheen.lineTo(at(0.55, 0));
    sheen.lineTo(at(0, 0.75));
    sheen.closeSubpath();
    painter->fillPath(sheen, QColor(255, 255, 255, dark ? 18 : 30));

    const qreal barHeight = std::max(2.0, snap(sh * 0.09, dpr));
    const QRectF bar(screen.left(), screen.bottom() - barHeight, sw, barHeight);
    painter->fillRect(bar, QColor(255, 255, 255, dark ? 45 : 70));
    painter->restore();

    if (!screenImage.isNull()) {
        drawImageIn(painter, screenImage, screen.adjusted(2, 2, -2, -barHeight - 2));
    }
    painter->restore();
}

void AvdResourceDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QPalette &palette = option.palette;
    const bool dark = isDark(palette);
    const bool hovered = option.state & QStyle::State_MouseOver;
    const bool selected = option.state & QStyle::State_Selected;
    const bool focused = option.state & QStyle::State_HasFocus;
    const bool openable = index.data(AvdResourceModel::LaunchableRole).toBool();
    const bool device = index.data(AvdResourceModel::CategoryRole).value<AvdResourceModel::Category>() == AvdResourceModel::Category::Devices;

    const QRectF card =
        QRectF(option.rect).adjusted(AvdResourceView::Gap / 2.0, AvdResourceView::Gap / 2.0, -AvdResourceView::Gap / 2.0, -AvdResourceView::Gap / 2.0);
    const QColor window = palette.color(QPalette::Window);
    const QColor text = palette.color(QPalette::Text);
    const QColor highlight = palette.color(QPalette::Highlight);
    QColor background = dark ? mix(window, text, 0.07) : palette.color(QPalette::Base);
    if (openable && (hovered || selected)) {
        background = mix(background, highlight, selected ? 0.18 : 0.09);
    }
    QColor border = mix(background, text, dark ? 0.12 : 0.16);
    if (selected || focused) {
        border = highlight;
    }
    painter->setPen(QPen(border, (selected || focused) ? 2 : 1));
    painter->setBrush(background);
    painter->drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), Radius, Radius);

    if (!openable) {
        // Visible and readable, but plainly not something to click yet.
        painter->setOpacity(0.72);
    }

    QRectF content = card.adjusted(Padding, Padding, -Padding, -Padding);
    qreal textLeft;
    if (device) {
        const qreal artHeight = content.height();
        const QRectF art(content.left(), content.top(), qRound(artHeight * 1.5), artHeight);
        const QImage serverImage = index.data(AvdResourceModel::ServerIconRole).value<QImage>();
        // A feed icon small enough sits on the monitor's screen; a bigger picture gets the frame, both at most their own size.
        const QSizeF natural = serverImage.deviceIndependentSize();
        const QRectF screen = desktopScreenRect(art);
        if (serverImage.isNull() || (natural.width() <= screen.width() - 4 && natural.height() <= screen.height() * 0.8)) {
            paintDesktopArt(painter, art, dark, serverImage);
        } else {
            painter->setPen(Qt::NoPen);
            painter->setBrush(mix(background, text, dark ? 0.08 : 0.05));
            painter->drawRoundedRect(art, 6, 6);
            drawImageIn(painter, serverImage, art);
        }
        textLeft = art.right() + Padding;
    } else {
        const int iconSize = 32;
        const QRect iconRect(int(content.left()), int(content.center().y() - iconSize / 2.0), iconSize, iconSize);
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        if (!icon.isNull()) {
            icon.paint(painter, iconRect, Qt::AlignCenter);
        } else {
            // No icon theme at all: a tile with the initial still tells the apps apart.
            const QString title = index.data().toString();
            const QColor tile = QColor::fromHsv(int(qHash(title) % 360), 110, dark ? 170 : 200);
            painter->setPen(Qt::NoPen);
            painter->setBrush(tile);
            painter->drawRoundedRect(iconRect, 6, 6);
            painter->setPen(Qt::white);
            QFont initialFont = option.font;
            initialFont.setBold(true);
            initialFont.setPixelSize(16);
            painter->setFont(initialFont);
            painter->drawText(iconRect, Qt::AlignCenter, title.left(1).toUpper());
        }
        textLeft = iconRect.right() + 1 + Padding;
    }

    QFont titleFont = option.font;
    titleFont.setWeight(QFont::DemiBold);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.08);
    const QFontMetricsF titleMetrics(titleFont);
    const QFontMetricsF subtitleMetrics(option.font);

    QString badge;
    if (!openable) {
        badge = index.data(AvdResourceModel::CategoryRole).value<AvdResourceModel::Category>() == AvdResourceModel::Category::Apps
            ? i18nc("@label badge on a remote application card", "Coming soon")
            : i18nc("@label badge on a resource card", "Not supported");
    }
    QFont badgeFont = option.font;
    badgeFont.setPointSizeF(badgeFont.pointSizeF() * 0.85);
    const QFontMetricsF badgeMetrics(badgeFont);
    qreal textRight = content.right();
    const qreal badgeWidth = badge.isEmpty() ? 0 : badgeMetrics.horizontalAdvance(badge) + 12;
    // The badge only goes where it leaves the title room.
    const bool showBadge = !badge.isEmpty() && textRight - textLeft - badgeWidth > 100;
    if (showBadge) {
        textRight -= badgeWidth + 8;
    }
    const qreal titleWidth = std::max<qreal>(0, textRight - textLeft);
    const qreal textWidth = std::max<qreal>(0, content.right() - textLeft);
    const qreal blockHeight = titleMetrics.height() + 4 + subtitleMetrics.height();
    const qreal top = content.center().y() - blockHeight / 2;

    painter->setFont(titleFont);
    painter->setPen(text);
    painter->drawText(QRectF(textLeft, top, titleWidth, titleMetrics.height()),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      titleMetrics.elidedText(index.data().toString(), Qt::ElideRight, titleWidth));
    painter->setFont(option.font);
    painter->setPen(mix(text, background, 0.35));
    painter->drawText(QRectF(textLeft, top + titleMetrics.height() + 4, textWidth, subtitleMetrics.height()),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      subtitleMetrics.elidedText(index.data(AvdResourceModel::SubtitleRole).toString(), Qt::ElideRight, textWidth));

    if (showBadge) {
        const QRectF pill(content.right() - badgeWidth, top + (titleMetrics.height() - badgeMetrics.height()) / 2 - 2, badgeWidth, badgeMetrics.height() + 4);
        painter->setPen(Qt::NoPen);
        painter->setBrush(mix(background, text, 0.12));
        painter->drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
        painter->setFont(badgeFont);
        painter->setPen(text);
        painter->drawText(pill, Qt::AlignCenter, badge);
    }
    painter->restore();
}

AvdResourceView::AvdResourceView(QWidget *parent)
    : QListView(parent)
{
    setViewMode(QListView::ListMode);
    setFlow(QListView::LeftToRight);
    setWrapping(true);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setAttribute(Qt::WA_Hover);
    setBackgroundRole(QPalette::Window);
    viewport()->setBackgroundRole(QPalette::Window);
    setItemDelegate(new AvdResourceDelegate(this));
    setAccessibleName(i18nc("@info accessible name", "Workspace resources"));
    updateGrid();
}

void AvdResourceView::setCategory(AvdResourceModel::Category category)
{
    m_category = category;
    updateGrid();
}

AvdResourceModel::Category AvdResourceView::category() const
{
    return m_category;
}

int AvdResourceView::columnCount() const
{
    return m_columns;
}

QSize AvdResourceView::cardSize() const
{
    return m_cardSize;
}

void AvdResourceView::resizeEvent(QResizeEvent *event)
{
    updateGrid();
    QListView::resizeEvent(event);
}

void AvdResourceView::updateGrid()
{
    const bool device = m_category == AvdResourceModel::Category::Devices;
    const int minWidth = device ? MinDeviceWidth : MinAppWidth;
    // Count columns against the width QListView itself wraps rows at, or a row can come up a card short:
    // the viewport at its widest less a scroll bar, shown or not, and the frame of styles that frame only the contents (Oxygen).
    int wrapWidth = maximumViewportSize().width() - style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, verticalScrollBar());
    if (style()->styleHint(QStyle::SH_ScrollView_FrameOnlyAroundContents, nullptr, this)) {
        QStyleOption option;
        option.initFrom(this);
        wrapWidth -= 2 * style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, this);
    }
    // A row wraps once its cards reach that width.
    const int available = std::max(minWidth, wrapWidth - 1);
    const int columns = std::max(1, available / minWidth);
    const int width = std::min(MaxCardWidth + Gap, available / columns);
    const QSize cell(width, (device ? DeviceCardHeight : AppCardHeight) + Gap);
    if (cell == m_cardSize && columns == m_columns) {
        return;
    }
    m_columns = columns;
    m_cardSize = cell;
    setGridSize(cell);
    scheduleDelayedItemsLayout();
}
