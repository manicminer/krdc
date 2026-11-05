/*
    SPDX-FileCopyrightText: 2008 Urs Wolfer <uwolfer@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef RDPVIEWFACTORY_H
#define RDPVIEWFACTORY_H

#include "remoteviewfactory.h"

#include "rdppreferences.h"
#include "rdpview.h"

#include <QPointer>

#include <memory>

class AvdWorkspaceDialog;
class QNetworkAccessManager;

namespace Avd
{
class TokenStore;
class WorkspaceCache;
}

class RdpViewFactory : public RemoteViewFactory
{
    Q_OBJECT

public:
    explicit RdpViewFactory(QObject *parent, const QVariantList &args);

    ~RdpViewFactory() override;

    bool supportsUrl(const QUrl &url) const override;

    ConnectionFile loadConnectionFile(const QUrl &url) const override;

    ConnectionLaunch loadLaunchFromFile(const QUrl &url, QString *errorMessage) const override;

    RemoteView *createView(QWidget *parent, const QUrl &url, KConfigGroup configGroup) override;

    RemoteView *createViewForLaunch(QWidget *parent, const ConnectionLaunch &launch, KConfigGroup configGroup) override;

    QList<QAction *> createActions(QWidget *parentWindow) override;

    HostPreferences *createHostPreferences(KConfigGroup configGroup, QWidget *parent) override;

    QString scheme() const override;

    QString connectActionText() const override;

    QString connectButtonText() const override;

    QString connectToolTipText() const override;

    bool importFileSettings(const QHash<QString, QString> &settings, KConfigGroup configGroup, QWidget *parent) const override;

private:
    void showWorkspaces(QWidget *parentWindow);

    QString m_connectToolTipString;
    QNetworkAccessManager *m_network = nullptr;
    std::unique_ptr<Avd::TokenStore> m_tokenStore;
    /** Shared by every workspace window of this KRDC, so reopening one uses the list fetched before. */
    std::unique_ptr<Avd::WorkspaceCache> m_workspaceCache;
    /** At most one workspace window: a sign-out in one could otherwise be undone by another's pending fetch. */
    QPointer<AvdWorkspaceDialog> m_workspaceDialog;
};

#endif // RDPVIEWFACTORY_H
