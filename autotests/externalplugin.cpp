/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

// A compatible plugin installed by someone other than this build. An installed
// KRDC offers it; a build tree, which only uses what it built, must not.

#include "remoteviewfactory.h"

class ExternalPlugin : public RemoteViewFactory
{
    Q_OBJECT

public:
    ExternalPlugin(QObject *parent, const QVariantList &)
        : RemoteViewFactory(parent)
    {
    }

    bool supportsUrl(const QUrl &url) const override
    {
        return url.scheme() == scheme();
    }
    RemoteView *createView(QWidget *, const QUrl &, KConfigGroup) override
    {
        return nullptr;
    }
    HostPreferences *createHostPreferences(KConfigGroup, QWidget *) override
    {
        return nullptr;
    }
    QString scheme() const override
    {
        return QStringLiteral("krdc-external-test");
    }
    QString connectActionText() const override
    {
        return scheme();
    }
    QString connectButtonText() const override
    {
        return scheme();
    }
    QString connectToolTipText() const override
    {
        return scheme();
    }
};

K_PLUGIN_CLASS_WITH_JSON(ExternalPlugin, "external.json")

#include "externalplugin.moc"
