/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "mainwindow.h"

#include <KAboutData>
#include <KActionCollection>
#include <KCModule>
#include <KPluginFactory>
#include <KPluginMetaData>

#include <QAction>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

namespace
{
// Files mapped into process @p pid whose path contains @p needle.
QSet<QString> mappedFiles(const QString &pid, const QString &needle)
{
    QSet<QString> files;
    QFile maps(QStringLiteral("/proc/%1/maps").arg(pid));
    if (!maps.open(QIODevice::ReadOnly)) {
        return files;
    }
    const QList<QByteArray> lines = maps.readAll().split('\n');
    for (const QByteArray &line : lines) {
        const qsizetype slash = line.indexOf('/');
        if (slash >= 0) {
            const QString path = QString::fromLocal8Bit(line.mid(slash));
            if (path.contains(needle)) {
                files.insert(QFileInfo(path).canonicalFilePath());
            }
        }
    }
    return files;
}

QString canonical(const QString &path)
{
    return QFileInfo(path).canonicalFilePath();
}

struct Startup {
    bool ready = false;
    bool survived = false;
    QSet<QString> plugins;
    QSet<QString> cores;
};

// Runs @p binary until its main window is ready and a little beyond,
// collecting the KRDC plugins and krdccore libraries it maps meanwhile.
Startup startKrdc(const QString &binary)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("KDE_DEBUG"), QStringLiteral("1")); // no crash dialog
    env.insert(QStringLiteral("QT_LOGGING_RULES"), QStringLiteral("*.debug=false;KRDC.debug=true"));
    env.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));

    QProcess krdc;
    krdc.setProcessEnvironment(env);
    krdc.setProcessChannelMode(QProcess::MergedChannels);
    krdc.start(binary, {});
    Startup startup;
    if (!krdc.waitForStarted()) {
        return startup;
    }
    const QString pid = QString::number(krdc.processId());
    QByteArray output;
    const auto sample = [&]() {
        startup.plugins.unite(mappedFiles(pid, QStringLiteral("/krdc_")));
        startup.cores.unite(mappedFiles(pid, QStringLiteral("/libkrdccore.so")));
        krdc.waitForReadyRead(50);
        output += krdc.readAll();
    };

    QDeadlineTimer readyDeadline(30000);
    while (krdc.state() == QProcess::Running && !readyDeadline.hasExpired() && !output.contains("KRDC ready")) {
        sample();
    }
    startup.ready = output.contains("KRDC ready");
    QDeadlineTimer settle(1500);
    while (krdc.state() == QProcess::Running && !settle.hasExpired()) {
        sample();
    }
    startup.survived = krdc.state() == QProcess::Running;
    krdc.kill();
    krdc.waitForFinished();
    output += krdc.readAll();

    qInfo().noquote() << binary << "ready" << startup.ready << "survived" << startup.survived << "exit code" << krdc.exitCode();
    qInfo() << "plugins:" << startup.plugins << "cores:" << startup.cores;
    if (!startup.survived) {
        qInfo().noquote() << output;
    }
    return startup;
}

// A copy of krdc in its own directory with only the RDP plugin next to it,
// laid out as built or installed. The copy still finds the built krdccore.
class Layout
{
public:
    Layout()
        : m_dir(QDir::home().filePath(QStringLiteral("layout-XXXXXX")))
    {
        const QString pluginDir = pluginPath(QString());
        m_valid = m_dir.isValid() && QDir().mkpath(bin()) && QDir().mkpath(pluginDir) && QFile::copy(QStringLiteral(KRDC_BINARY), binary())
            && QFile::copy(QStringLiteral(KRDC_RDP_PLUGIN), pluginPath(QStringLiteral("krdc_rdpplugin.so")));
    }
    bool isValid() const
    {
        return m_valid;
    }
    QString bin() const
    {
        return m_dir.filePath(QStringLiteral("bin"));
    }
    QString binary() const
    {
        return QDir(bin()).filePath(QStringLiteral("krdc"));
    }
    QString pluginPath(const QString &fileName) const
    {
        return QDir::cleanPath(bin() + QLatin1Char('/') + QStringLiteral(KRDC_RELATIVE_PLUGINDIR) + QStringLiteral("/krdc/") + fileName);
    }
    // Marks the layout as a build tree that built @p fileNames.
    bool writeManifest(const QStringList &fileNames) const
    {
        QFile manifest(QDir(bin()).filePath(QStringLiteral("krdc-build-plugins")));
        if (!manifest.open(QIODevice::WriteOnly)) {
            return false;
        }
        for (const QString &fileName : fileNames) {
            manifest.write(QDir(bin()).relativeFilePath(pluginPath(fileName)).toUtf8() + '\n');
        }
        return true;
    }

private:
    QTemporaryDir m_dir;
    bool m_valid = false;
};
}

// QT_PLUGIN_PATH lists stale plugins that abort when loaded and a compatible
// external one, ahead of Qt's plugin directory, which may hold the plugins of
// an installed KRDC.
class KrdcStartupTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void binaryStartsWithBuiltPlugins();
    void buildTreeSkipsPluginsNoLongerBuilt();
    void installedKrdcOffersExternalPlugins();
    void mainWindowOffersPluginActions();
};

void KrdcStartupTest::initTestCase()
{
#ifndef Q_OS_LINUX
    QSKIP("Needs /proc to see which libraries KRDC loaded");
#endif
    QVERIFY(qEnvironmentVariable("QT_PLUGIN_PATH").contains(QLatin1String("stale-plugins")));
    QVERIFY(qEnvironmentVariable("QT_PLUGIN_PATH").contains(QLatin1String("external-plugins")));
}

// What running bin/krdc from the build tree does.
void KrdcStartupTest::binaryStartsWithBuiltPlugins()
{
    const Startup startup = startKrdc(QStringLiteral(KRDC_BINARY));
    QVERIFY(startup.ready);
    QVERIFY(startup.survived);
    const QString builtPlugin = canonical(QStringLiteral(KRDC_RDP_PLUGIN));
    QVERIFY(startup.plugins.contains(builtPlugin));
    for (const QString &plugin : startup.plugins) {
        QCOMPARE(QFileInfo(plugin).path(), QFileInfo(builtPlugin).path());
    }
    QCOMPARE(startup.cores, QSet<QString>{canonical(QStringLiteral(KRDC_CORE_LIBRARY))});
}

// A protocol turned off after it was built leaves its plugin behind.
void KrdcStartupTest::buildTreeSkipsPluginsNoLongerBuilt()
{
    Layout layout;
    QVERIFY(layout.isValid());
    QVERIFY(QFile::copy(QStringLiteral(STALE_TOGGLED_PLUGIN), layout.pluginPath(QStringLiteral("krdc_vncplugin.so"))));
    QVERIFY(layout.writeManifest({QStringLiteral("krdc_rdpplugin.so")}));

    const Startup startup = startKrdc(layout.binary());
    QVERIFY(startup.ready);
    QVERIFY(startup.survived);
    QCOMPARE(startup.plugins, QSet<QString>{canonical(layout.pluginPath(QStringLiteral("krdc_rdpplugin.so")))});
    QCOMPARE(startup.cores, QSet<QString>{canonical(QStringLiteral(KRDC_CORE_LIBRARY))});
}

void KrdcStartupTest::installedKrdcOffersExternalPlugins()
{
    Layout layout;
    QVERIFY(layout.isValid());

    const Startup startup = startKrdc(layout.binary());
    QVERIFY(startup.ready);
    QVERIFY(startup.survived);
    QCOMPARE(startup.plugins, (QSet<QString>{canonical(layout.pluginPath(QStringLiteral("krdc_rdpplugin.so"))), canonical(QStringLiteral(EXTERNAL_PLUGIN))}));
    QCOMPARE(startup.cores, QSet<QString>{canonical(QStringLiteral(KRDC_CORE_LIBRARY))});
}

void KrdcStartupTest::mainWindowOffersPluginActions()
{
    KAboutData about(QStringLiteral("krdc"), QStringLiteral("KRDC"), QStringLiteral("test"));
    KAboutData::setApplicationData(about);

    MainWindow window;

    QAction *workspaces = window.actionCollection()->action(QStringLiteral("rdp_avd_workspaces"));
    QVERIFY2(workspaces, "the RDP plugin's actions are missing");
    const QList<QObject *> shownIn = workspaces->associatedObjects();
    QVERIFY(std::any_of(shownIn.cbegin(), shownIn.cend(), [](QObject *object) {
        return qobject_cast<QMenu *>(object);
    }));

    // Only what this build built is offered, here and in the settings.
    const QString builtPluginDir = QFileInfo(canonical(QStringLiteral(KRDC_RDP_PLUGIN))).path();
    KPluginMetaData rdp;
    const QList<KPluginMetaData> offered = RemoteViewFactory::compatiblePlugins();
    for (const KPluginMetaData &plugin : offered) {
        QCOMPARE(QFileInfo(canonical(plugin.fileName())).path(), builtPluginDir);
        if (plugin.pluginId() == QLatin1String("krdc_rdpplugin")) {
            rdp = plugin;
        }
    }
    QVERIFY(rdp.isValid());

    // The settings page of the plugin, found by name as KPluginWidget does.
    const KPluginMetaData kcm(rdp.value(QStringLiteral("X-KDE-ConfigModule")), KPluginMetaData::AllowEmptyMetaData);
    QVERIFY(kcm.isValid());
    QCOMPARE(QFileInfo(canonical(kcm.fileName())).path(), builtPluginDir + QStringLiteral("/kcms"));
    QWidget parent;
    const auto module = KPluginFactory::instantiatePlugin<KCModule>(kcm, &parent);
    QVERIFY2(module, qPrintable(module.errorText));
    module.plugin->load();
    QVERIFY(module.plugin->widget());

    const QSet<QString> cores = mappedFiles(QStringLiteral("self"), QStringLiteral("/libkrdccore.so"));
    QCOMPARE(cores, QSet<QString>{canonical(QStringLiteral(KRDC_CORE_LIBRARY))});
}

QTEST_MAIN(KrdcStartupTest)

#include "krdcstartuptest.moc"
