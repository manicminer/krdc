/*
    SPDX-FileCopyrightText: 2008 Urs Wolfer <uwolfer@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef REMOTEVIEWFACTORY_H
#define REMOTEVIEWFACTORY_H

#include "connectionlaunch.h"
#include "krdccore_export.h"
#include "remoteview.h"

#include <KConfigGroup>
#include <KPluginFactory>
#include <KPluginMetaData>

class QAction;

/**
 * Version of the plugin interface. Plugins state it as "X-KDE-KRDC-PluginVersion"
 * in their metadata. Bump both whenever RemoteViewFactory or RemoteView change
 * incompatibly.
 */
#define KRDC_PLUGIN_VERSION 2

#include <QHash>

/**
 * Factory to be implemented by any plugin.
 */
class KRDCCORE_EXPORT RemoteViewFactory : public QObject
{
    Q_OBJECT

public:
    struct ConnectionFile {
        /**
         * The connection that the file describes. Invalid if the plugin cannot load the file.
         */
        QUrl url;
        /**
         * The settings in the file, for importFileSettings(). The plugin defines the keys.
         */
        QHash<QString, QString> settings;
    };

    /**
     * Deconstructor.
     */
    ~RemoteViewFactory() override;

    /**
     * Returns the installed plugins built for this KRDC's plugin interface.
     * Plugins of another version are skipped without loading them. Run from
     * a build directory, only the plugins built there are returned.
     */
    static QList<KPluginMetaData> compatiblePlugins();

    /**
     * Returns true if the provided @p url is supported by the current plugin.
     */
    virtual bool supportsUrl(const QUrl &url) const = 0;

    /**
     * Tries to load connection parameters from the file at @p url.
     */
    virtual ConnectionFile loadConnectionFile(const QUrl &url) const;

    /**
     * Tries to load a connection from a file. Unlike loadConnectionFile(), this
     * can keep settings that a URL cannot carry. The default implementation
     * wraps loadConnectionFile(), keeping its settings for importFileSettings().
     *
     * Returns an invalid launch if the file is not handled by this plugin. If
     * the plugin handles the file type but cannot use this file, @p errorMessage
     * is set to the reason.
     */
    virtual ConnectionLaunch loadLaunchFromFile(const QUrl &url, QString *errorMessage) const;

    /**
     * Returns a new RemoteView implementing object.
     */
    virtual RemoteView *createView(QWidget *parent, const QUrl &url, KConfigGroup configGroup) = 0;

    /**
     * Returns a new RemoteView for @p launch. The default implementation
     * ignores the payload and uses the URL.
     */
    virtual RemoteView *createViewForLaunch(QWidget *parent, const ConnectionLaunch &launch, KConfigGroup configGroup);

    /**
     * Actions this plugin offers in the main window, for example to browse a
     * remote workspace. Starting a connection from them emits launchRequested().
     */
    virtual QList<QAction *> createActions(QWidget *parentWindow);

Q_SIGNALS:
    /** Asks the application to start @p launch in a new tab. */
    void launchRequested(const ConnectionLaunch &launch);

public:
    /**
     * Returns a new HostPreferences implementing object or 0 if no settings are available.
     */
    virtual HostPreferences *createHostPreferences(KConfigGroup configGroup, QWidget *parent) = 0;

    /**
     * Returns the supported scheme.
     * @see QUrl::scheme()
     */
    virtual QString scheme() const = 0;

    /**
     * Returns the text of the action in the file menu (by default).
     */
    virtual QString connectActionText() const = 0;

    /**
     * Returns the text of the connect button on the startscreen.
     */
    virtual QString connectButtonText() const = 0;

    /**
     * Returns the tooltip next to the url navigator when the user presses
     * the connect action.
     */
    virtual QString connectToolTipText() const = 0;

    /**
     * Imports @p settings, from the ConnectionFile that loadConnectionFile() returned or the
     * ConnectionLaunch::fileSettings of loadLaunchFromFile(), into
     * @p configGroup, the settings of the host that it names.
     * The file is untrusted, so ask the user before importing a setting that is not safe for any
     * file to set, with @p parent as the parent of the dialog.
     * Returns false if the connection should not go ahead.
     */
    virtual bool importFileSettings(const QHash<QString, QString> &settings, KConfigGroup configGroup, QWidget *parent) const;

protected:
    /**
     * Protected constructor so it cannot be instantiated.
     */
    RemoteViewFactory(QObject *parent = nullptr);
};

#endif // REMOTEVIEWFACTORY_H
