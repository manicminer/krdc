/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

// Stands in for a plugin a build tree built before its protocol was turned
// off. It is compatible but no longer part of the build, so KRDC must not even
// load it; if it does, the test aborts here.

#include <KPluginFactory>

#include <QObject>

namespace
{
struct AbortOnLoad {
    AbortOnLoad()
    {
        qFatal("KRDC loaded an incompatible stale plugin");
    }
} abortOnLoad;
}

class StaleToggledPlugin : public QObject
{
    Q_OBJECT

public:
    StaleToggledPlugin(QObject *parent, const QVariantList &)
        : QObject(parent)
    {
    }
};

K_PLUGIN_CLASS_WITH_JSON(StaleToggledPlugin, "stale_toggled.json")

#include "staletoggledplugin.moc"
