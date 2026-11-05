/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include "avdfeed.h"

#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QListView>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>

/** The resources of the workspaces, one row each. */
class AvdResourceModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum class Category {
        Devices,
        Apps,
    };
    Q_ENUM(Category)

    enum Role {
        SubtitleRole = Qt::UserRole + 1,
        CategoryRole,
        LaunchableRole,
        KeyRole,
        /** The icon the feed offered, if it was loaded. */
        ServerIconRole,
    };

    explicit AvdResourceModel(QObject *parent = nullptr);

    void setResources(const QList<Avd::Resource> &resources, const QHash<QString, QImage> &icons = {});
    void setIcon(const QString &key, const QImage &icon);
    void clear();

    QList<Avd::Resource> resources() const;
    std::optional<Avd::Resource> resource(const QString &key) const;
    int count(Category category) const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    static Category categoryOf(const Avd::Resource &resource);
    /** Only desktops can be opened so far; this does not need their address yet. */
    static bool canOpen(const Avd::Resource &resource);
    /** A themed icon matching the name of a remote application, or a generic one. */
    static QIcon themeIconFor(const QString &title);

private:
    QList<Avd::Resource> m_resources;
    QHash<QString, QImage> m_icons;
};

/** Rows of one category, matching the search text, sorted by title. */
class AvdResourceFilter : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit AvdResourceFilter(QObject *parent = nullptr);

    void setCategory(AvdResourceModel::Category category);
    AvdResourceModel::Category category() const;
    void setSearchText(const QString &text);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    AvdResourceModel::Category m_category = AvdResourceModel::Category::Devices;
    QString m_search;
};

/** Paints a resource as a card: desktop artwork or application icon, title and workspace. */
class AvdResourceDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit AvdResourceDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    /**
     * A monitor standing for a remote desktop, drawn as vectors at the painter's scale.
     * A @p screenImage is shown on its screen, never larger than its own logical size.
     */
    static void paintDesktopArt(QPainter *painter, const QRectF &rect, bool dark, const QImage &screenImage = {});
    /** Where the monitor's screen is within @p rect. */
    static QRectF desktopScreenRect(const QRectF &rect);
};

/**
 * Cards in as many columns as fit the width, scrolling vertically only.
 * Desktops get wider, taller cards than applications.
 */
class AvdResourceView : public QListView
{
    Q_OBJECT

public:
    static constexpr int Gap = 12;
    static constexpr int MinDeviceWidth = 300;
    static constexpr int MinAppWidth = 220;
    static constexpr int MaxCardWidth = 480;

    explicit AvdResourceView(QWidget *parent = nullptr);

    void setCategory(AvdResourceModel::Category category);
    AvdResourceModel::Category category() const;

    int columnCount() const;
    QSize cardSize() const;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateGrid();

    AvdResourceModel::Category m_category = AvdResourceModel::Category::Devices;
    int m_columns = 1;
    QSize m_cardSize;
};
