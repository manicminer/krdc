/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

// Stands in for a KRDC plugin installed by an older release. Its metadata
// lacks the current plugin version, so KRDC must not even load it; if it
// does, the test aborts here instead of running a mismatched interface.

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

class StaleRdpPlugin : public QObject
{
    Q_OBJECT

public:
    StaleRdpPlugin(QObject *parent, const QVariantList &)
        : QObject(parent)
    {
    }
};

K_PLUGIN_CLASS_WITH_JSON(StaleRdpPlugin, "stale_rdp.json")

#include "stalerdpplugin.moc"
