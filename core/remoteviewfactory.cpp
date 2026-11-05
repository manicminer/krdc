/*
    SPDX-FileCopyrightText: 2008 Urs Wolfer <uwolfer@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "remoteviewfactory.h"

#include "krdc_debug.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

namespace
{
// Checked on metadata only: loading a mismatched plugin would also load the
// krdccore it was built against, and calls into it crash.
bool isCompatible(const KPluginMetaData &plugin)
{
    const int version = plugin.value(QStringLiteral("X-KDE-KRDC-PluginVersion"), 0);
    if (version != KRDC_PLUGIN_VERSION) {
        qCWarning(KRDC) << "Skipping plugin" << plugin.fileName() << "with plugin version" << version << "instead of" << KRDC_PLUGIN_VERSION;
        return false;
    }
    return true;
}
}

QList<KPluginMetaData> RemoteViewFactory::compatiblePlugins()
{
    const QDir executableDir(QCoreApplication::applicationDirPath());
    // Search the plugins installed (or built) along with this executable
    // before Qt's plugin directory, which may hold those of another KRDC.
    // Configuration modules are found through this path as well.
    QCoreApplication::addLibraryPath(executableDir.filePath(QStringLiteral(KRDC_RELATIVE_PLUGINDIR)));

    // A build tree lists what it built, relative to the executable, and uses
    // exactly that: not plugins of protocols since turned off, nor any found
    // in the plugin path.
    QFile buildManifest(executableDir.filePath(QStringLiteral("krdc-build-plugins")));
    if (!buildManifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return KPluginMetaData::findPlugins(QStringLiteral("krdc"), isCompatible);
    }
    QList<KPluginMetaData> plugins;
    while (!buildManifest.atEnd()) {
        const QString file = QString::fromUtf8(buildManifest.readLine()).trimmed();
        if (file.isEmpty() || !QFile::exists(executableDir.filePath(file))) {
            continue;
        }
        const KPluginMetaData plugin(executableDir.filePath(file));
        if (plugin.isValid() && isCompatible(plugin)) {
            plugins << plugin;
        }
    }
    return plugins;
}

RemoteViewFactory::RemoteViewFactory(QObject *parent)
    : QObject(parent)
{
}

RemoteViewFactory::~RemoteViewFactory()
{
}

RemoteViewFactory::ConnectionFile RemoteViewFactory::loadConnectionFile(const QUrl &url) const
{
    Q_UNUSED(url);
    return {};
}

bool RemoteViewFactory::importFileSettings(const QHash<QString, QString> &settings, KConfigGroup configGroup, QWidget *parent) const
{
    Q_UNUSED(settings);
    Q_UNUSED(configGroup);
    Q_UNUSED(parent);
    return true;
}

ConnectionLaunch RemoteViewFactory::loadLaunchFromFile(const QUrl &url, QString *errorMessage) const
{
    Q_UNUSED(errorMessage);
    ConnectionFile file = loadConnectionFile(url);
    if (!file.url.isValid() || file.url.isEmpty()) {
        return {};
    }
    ConnectionLaunch launch = ConnectionLaunch::fromUrl(file.url);
    launch.fromFile = true;
    launch.fileSettings = std::move(file.settings);
    return launch;
}

RemoteView *RemoteViewFactory::createViewForLaunch(QWidget *parent, const ConnectionLaunch &launch, KConfigGroup configGroup)
{
    return createView(parent, launch.url, configGroup);
}

QList<QAction *> RemoteViewFactory::createActions(QWidget *parentWindow)
{
    Q_UNUSED(parentWindow);
    return {};
}
