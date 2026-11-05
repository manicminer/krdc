/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdhandlerregistration.h"

#include <KApplicationTrader>
#include <KConfigGroup>
#include <KService>
#include <KSharedConfig>
#include <KSycoca>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <sys/stat.h>
#include <unistd.h>

using namespace Avd;

namespace
{
const QString Mime = QString::fromLatin1(HandlerRegistration::MimeType);

QString applicationsDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
}

void writeDesktopFile(const QString &name, const QByteArray &contents)
{
    QVERIFY(QDir().mkpath(applicationsDir()));
    QFile file(applicationsDir() + QLatin1Char('/') + name);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(contents);
}

// KSycoca re-reads changed files at most every 1.5 seconds per thread, but at once in a new one.
void inFreshThread(const std::function<void()> &function)
{
    std::unique_ptr<QThread> thread(QThread::create([&function]() {
        KSycoca::disableAutoRebuild();
        KSycoca::self()->ensureCacheValid();
        function();
    }));
    thread->start();
    thread->wait();
}

void makeDefault(const QString &desktopFile)
{
    bool found = false;
    inFreshThread([&]() {
        const KService::Ptr service = KService::serviceByStorageId(desktopFile);
        found = service;
        if (service) {
            KApplicationTrader::setPreferredService(Mime, service);
        }
    });
    QVERIFY(found);
}

struct Handler {
    QString storageId;
    QString exec;
    bool noDisplay = false;
};

Handler currentHandler()
{
    Handler handler;
    inFreshThread([&handler]() {
        if (const KService::Ptr service = KApplicationTrader::preferredService(Mime)) {
            handler = {service->storageId(), service->exec(), service->noDisplay()};
        }
    });
    return handler;
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<unreadable>");
}

void writeBytes(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(contents), contents.size());
}

QString mimeappsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/mimeapps.list");
}

QString ourDesktopFile()
{
    return applicationsDir() + QLatin1Char('/') + QLatin1String(HandlerRegistration::DesktopFileName);
}

// Unrelated associations, with comments and spacing a rewrite would lose.
const QByteArray UserMimeapps =
    "# my associations\n[Added Associations]\ntext/plain=org.kde.kate.desktop;\n\n[Default Applications]\n"
    "text/html = firefox.desktop\nx-scheme-handler/ms-appx-web=test.Previous.desktop\n";

constexpr QFile::Permissions Rwx = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
constexpr QFile::Permissions Rx = QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther;

// A copy of the helper in <base>/grand/parent/, the directories with the given permissions.
QString helperBelow(const QString &base, QFile::Permissions grand)
{
    const QString parent = base + QStringLiteral("/grand/parent");
    if (!QDir().mkpath(parent) || !QFile::setPermissions(parent, Rwx | Rx)
        || !QFile::copy(QStringLiteral(KRDC_AVD_HANDLER_PATH), parent + QStringLiteral("/handler"))
        || !QFile::setPermissions(parent + QStringLiteral("/handler"), Rwx | Rx) || !QFile::setPermissions(base + QStringLiteral("/grand"), grand)) {
        return {};
    }
    return parent + QStringLiteral("/handler");
}

bool setMode(const QString &path, mode_t mode)
{
    return chmod(QFile::encodeName(path).constData(), mode) == 0;
}

QString mimeappsDefault()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("mimeapps.list"), KConfig::NoGlobals, QStandardPaths::GenericConfigLocation);
    config->reparseConfiguration();
    return config->group(QStringLiteral("Default Applications")).readXdgListEntry(Mime).value(0);
}
}

class AvdHandlerRegistrationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void defaultHandlerIsBuilt();
    void nothingRegisteredImplicitly();
    void registersOnRequest();
    void reportsOtherHandler();
    void reportsOtherKrdcHandler();
    void refusesUnusableHelper_data();
    void refusesUnusableHelper();
    void reportsOverriddenDefault();
    void refusesUnsafeAncestor_data();
    void refusesUnsafeAncestor();
    void acceptsStickyAncestor();
    void revalidatesBeforeRegistering();
    void unreadableFilesAreNotTouched_data();
    void unreadableFilesAreNotTouched();
    void failedRegistrationRestoresBytes();
    void reportsFailedRestore();
};

void AvdHandlerRegistrationTest::initTestCase()
{
    // Keeps the user's associations and applications out of reach, also when run by hand.
    QStandardPaths::setTestModeEnabled(true);
    QVERIFY(qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS") == QLatin1String("disabled:"));
}

void AvdHandlerRegistrationTest::init()
{
    // Emptied rather than removed: KSycoca notices changed files and directories, not missing ones.
    QDir(applicationsDir()).removeRecursively();
    QVERIFY(QDir().mkpath(applicationsDir()));
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QVERIFY(QDir().mkpath(config));
    for (const QString &name : {QStringLiteral("/mimeapps.list"), QStringLiteral("/kde-mimeapps.list")}) {
        QFile file(config + name);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    }
}

void AvdHandlerRegistrationTest::defaultHandlerIsBuilt()
{
    // In the build tree the helper sits next to the executables.
    QCOMPARE(QFileInfo(HandlerRegistration::defaultHandlerPath()).canonicalFilePath(), QFileInfo(QStringLiteral(KRDC_AVD_HANDLER_PATH)).canonicalFilePath());
}

void AvdHandlerRegistrationTest::nothingRegisteredImplicitly()
{
    HandlerRegistration registration(QStringLiteral(KRDC_AVD_HANDLER_PATH));
    const auto status = registration.status();
    QCOMPARE(status.state, HandlerRegistration::State::NotRegistered);
    QVERIFY(status.currentName.isEmpty());
    QVERIFY(!QFile::exists(applicationsDir() + QLatin1Char('/') + QLatin1String(HandlerRegistration::DesktopFileName)));
    QVERIFY(mimeappsDefault().isEmpty());
}

void AvdHandlerRegistrationTest::registersOnRequest()
{
    HandlerRegistration registration(QStringLiteral(KRDC_AVD_HANDLER_PATH));
    QString error;
    QVERIFY2(registration.registerHandler(&error), qPrintable(error));
    QCOMPARE(registration.status().state, HandlerRegistration::State::Registered);
    QCOMPARE(mimeappsDefault(), QString::fromLatin1(HandlerRegistration::DesktopFileName));

    const Handler handler = currentHandler();
    QCOMPARE(handler.storageId, QString::fromLatin1(HandlerRegistration::DesktopFileName));
    QCOMPARE(handler.exec, QFileInfo(QStringLiteral(KRDC_AVD_HANDLER_PATH)).canonicalFilePath() + QStringLiteral(" %u"));
    QVERIFY(handler.noDisplay);
    // Registering for nothing else, in particular not for web links.
    const KSharedConfig::Ptr desktop =
        KSharedConfig::openConfig(applicationsDir() + QLatin1Char('/') + QLatin1String(HandlerRegistration::DesktopFileName), KConfig::SimpleConfig);
    QCOMPARE(desktop->group(QStringLiteral("Desktop Entry")).readXdgListEntry("MimeType"), QStringList{Mime});

    // Registering again changes nothing.
    QVERIFY(registration.registerHandler(&error));
    QCOMPARE(registration.status().state, HandlerRegistration::State::Registered);
}

void AvdHandlerRegistrationTest::reportsOtherHandler()
{
    writeDesktopFile(QStringLiteral("test.OtherBroker.desktop"),
                     "[Desktop Entry]\nType=Application\nName=Other Broker\nExec=/usr/bin/false %u\nMimeType=x-scheme-handler/ms-appx-web;\nNoDisplay=true\n");
    makeDefault(QStringLiteral("test.OtherBroker.desktop"));

    HandlerRegistration registration(QStringLiteral(KRDC_AVD_HANDLER_PATH));
    const auto status = registration.status();
    QCOMPARE(status.state, HandlerRegistration::State::OtherHandler);
    QCOMPARE(status.currentName, QStringLiteral("Other Broker"));
    QCOMPARE(status.currentDesktopFile, QStringLiteral("test.OtherBroker.desktop"));
    QCOMPARE(status.currentCommand, QStringLiteral("/usr/bin/false %u"));
    // Asking for the status never takes over.
    QCOMPARE(mimeappsDefault(), QStringLiteral("test.OtherBroker.desktop"));

    // Only replaced when asked to.
    QString error;
    QVERIFY(registration.registerHandler(&error));
    QCOMPARE(registration.status().state, HandlerRegistration::State::Registered);
    QVERIFY(QFile::exists(applicationsDir() + QStringLiteral("/test.OtherBroker.desktop")));
}

void AvdHandlerRegistrationTest::reportsOtherKrdcHandler()
{
    // For example the helper of a build tree, while running the installed KRDC.
    writeDesktopFile(QString::fromLatin1(HandlerRegistration::DesktopFileName),
                     "[Desktop Entry]\nType=Application\nName=KRDC\nExec=/nonexistent/krdc-avd-signin-handler %u\nMimeType=x-scheme-handler/ms-appx-web;\n");
    makeDefault(QString::fromLatin1(HandlerRegistration::DesktopFileName));

    HandlerRegistration registration(QStringLiteral(KRDC_AVD_HANDLER_PATH));
    const auto status = registration.status();
    QCOMPARE(status.state, HandlerRegistration::State::OtherKrdcHandler);
    QCOMPARE(status.currentCommand, QStringLiteral("/nonexistent/krdc-avd-signin-handler %u"));

    QString error;
    QVERIFY(registration.registerHandler(&error));
    QCOMPARE(registration.status().state, HandlerRegistration::State::Registered);
}

void AvdHandlerRegistrationTest::refusesUnusableHelper_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QFile::Permissions>("permissions");
    const QFile::Permissions executable = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup;
    QTest::newRow("missing") << QString() << QFile::Permissions();
    QTest::newRow("not executable") << QStringLiteral("handler") << (QFile::ReadOwner | QFile::WriteOwner);
    QTest::newRow("writable by others") << QStringLiteral("handler") << (executable | QFile::WriteOther);
    QTest::newRow("space in path") << QStringLiteral("my handler") << executable;
    QTest::newRow("field code in path") << QStringLiteral("handler%u") << executable;
}

void AvdHandlerRegistrationTest::refusesUnusableHelper()
{
    QFETCH(QString, name);
    QFETCH(QFile::Permissions, permissions);
    QTemporaryDir dir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/handler-XXXXXX"));
    QVERIFY(dir.isValid());
    QString path = dir.filePath(QStringLiteral("absent"));
    if (!name.isEmpty()) {
        path = dir.filePath(name);
        QVERIFY(QFile::copy(QStringLiteral(KRDC_AVD_HANDLER_PATH), path));
        QVERIFY(QFile::setPermissions(path, permissions));
    }

    HandlerRegistration registration(path);
    const auto status = registration.status();
    QCOMPARE(status.state, HandlerRegistration::State::HandlerUnusable);
    QVERIFY(!status.problem.isEmpty());
    QString error;
    QVERIFY(!registration.registerHandler(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(applicationsDir() + QLatin1Char('/') + QLatin1String(HandlerRegistration::DesktopFileName)));
    QVERIFY(mimeappsDefault().isEmpty());
}

void AvdHandlerRegistrationTest::reportsOverriddenDefault()
{
    // A desktop-specific association wins over mimeapps.list; registering must not claim success then.
    writeDesktopFile(QStringLiteral("test.OtherBroker.desktop"),
                     "[Desktop Entry]\nType=Application\nName=Other Broker\nExec=/usr/bin/false %u\nMimeType=x-scheme-handler/ms-appx-web;\n");
    QVERIFY(QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)));
    const QString desktopSpecific = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kde-mimeapps.list");
    QFile file(desktopSpecific);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("[Default Applications]\nx-scheme-handler/ms-appx-web=test.OtherBroker.desktop\n");
    file.close();

    const QByteArray desktop = qgetenv("XDG_CURRENT_DESKTOP");
    qputenv("XDG_CURRENT_DESKTOP", "KDE");
    HandlerRegistration registration(QStringLiteral(KRDC_AVD_HANDLER_PATH));
    if (registration.status().state != HandlerRegistration::State::OtherHandler) {
        qputenv("XDG_CURRENT_DESKTOP", desktop);
        QSKIP("This KService does not read desktop-specific associations");
    }
    QString error;
    QVERIFY(!registration.registerHandler(&error));
    QVERIFY(error.contains(QStringLiteral("Other Broker")));
    QCOMPARE(registration.status().state, HandlerRegistration::State::OtherHandler);
    qputenv("XDG_CURRENT_DESKTOP", desktop);
}

void AvdHandlerRegistrationTest::refusesUnsafeAncestor_data()
{
    QTest::addColumn<QFile::Permissions>("grand");
    QTest::newRow("writable by others") << (Rwx | Rx | QFile::WriteOther);
    QTest::newRow("writable by group") << (Rwx | Rx | QFile::WriteGroup);
}

void AvdHandlerRegistrationTest::refusesUnsafeAncestor()
{
    // Whoever can rename the grandparent can put another helper in its place.
    QFETCH(QFile::Permissions, grand);
    QTemporaryDir dir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/handler-XXXXXX"));
    QVERIFY(dir.isValid());
    const QString helper = helperBelow(dir.path(), grand);
    QVERIFY(!helper.isEmpty());

    HandlerRegistration registration(helper);
    QCOMPARE(registration.status().state, HandlerRegistration::State::HandlerUnusable);
    QString error;
    QVERIFY(!registration.registerHandler(&error));
    QVERIFY(!QFile::exists(ourDesktopFile()));
    QVERIFY(mimeappsDefault().isEmpty());
}

void AvdHandlerRegistrationTest::acceptsStickyAncestor()
{
    // Like /tmp: others cannot rename or remove what this user owns in it.
    QTemporaryDir dir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/handler-XXXXXX"));
    QVERIFY(dir.isValid());
    const QString helper = helperBelow(dir.path(), Rwx | Rx);
    QVERIFY(!helper.isEmpty());
    QVERIFY(setMode(dir.path() + QStringLiteral("/grand"), 01777));

    HandlerRegistration registration(helper);
    QVERIFY2(registration.status().state != HandlerRegistration::State::HandlerUnusable, qPrintable(registration.status().problem));
}

void AvdHandlerRegistrationTest::revalidatesBeforeRegistering()
{
    QTemporaryDir dir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/handler-XXXXXX"));
    QVERIFY(dir.isValid());
    const QString helper = helperBelow(dir.path(), Rwx | Rx);
    QVERIFY(!helper.isEmpty());
    HandlerRegistration registration(helper);
    QCOMPARE(registration.status().state, HandlerRegistration::State::NotRegistered);

    // Became unsafe while the dialog asked the user.
    QVERIFY(setMode(dir.path() + QStringLiteral("/grand"), 0777));
    QString error;
    QVERIFY(!registration.registerHandler(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(ourDesktopFile()));
    QVERIFY(mimeappsDefault().isEmpty());
}

void AvdHandlerRegistrationTest::unreadableFilesAreNotTouched_data()
{
    QTest::addColumn<bool>("desktopFile");
    QTest::newRow("mimeapps.list") << false;
    QTest::newRow("desktop file") << true;
}

void AvdHandlerRegistrationTest::unreadableFilesAreNotTouched()
{
    if (geteuid() == 0) {
        QSKIP("root reads everything");
    }
    QFETCH(bool, desktopFile);
    writeBytes(mimeappsPath(), UserMimeapps);
    const QByteArray oldDesktop = "[Desktop Entry]\nType=Application\nName=Mine\nExec=/usr/bin/false %u\n";
    writeDesktopFile(QString::fromLatin1(HandlerRegistration::DesktopFileName), oldDesktop);
    const QString unreadable = desktopFile ? ourDesktopFile() : mimeappsPath();
    QVERIFY(setMode(unreadable, 0));

    HandlerRegistration registration(QStringLiteral(KRDC_AVD_HANDLER_PATH));
    QString error;
    const bool registered = registration.registerHandler(&error);
    QVERIFY(setMode(unreadable, 0600));
    QVERIFY(!registered);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(mimeappsPath()), UserMimeapps);
    QCOMPARE(readBytes(ourDesktopFile()), oldDesktop);
}

void AvdHandlerRegistrationTest::failedRegistrationRestoresBytes()
{
    // As reportsOverriddenDefault, with files of the user that the attempt changed on the way.
    writeDesktopFile(QStringLiteral("test.OtherBroker.desktop"),
                     "[Desktop Entry]\nType=Application\nName=Other Broker\nExec=/usr/bin/false %u\nMimeType=x-scheme-handler/ms-appx-web;\n");
    writeBytes(mimeappsPath(), UserMimeapps);
    const QByteArray oldDesktop = "[Desktop Entry]\nType=Application\nName=Old KRDC\nExec=/nonexistent/krdc-avd-signin-handler %u\n# kept\n";
    writeDesktopFile(QString::fromLatin1(HandlerRegistration::DesktopFileName), oldDesktop);
    const QString desktopSpecific = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kde-mimeapps.list");
    writeBytes(desktopSpecific, "[Default Applications]\nx-scheme-handler/ms-appx-web=test.OtherBroker.desktop\n");

    const QByteArray desktop = qgetenv("XDG_CURRENT_DESKTOP");
    qputenv("XDG_CURRENT_DESKTOP", "KDE");
    HandlerRegistration registration(QStringLiteral(KRDC_AVD_HANDLER_PATH));
    if (registration.status().state != HandlerRegistration::State::OtherHandler) {
        qputenv("XDG_CURRENT_DESKTOP", desktop);
        QSKIP("This KService does not read desktop-specific associations");
    }
    QString error;
    const bool registered = registration.registerHandler(&error);
    qputenv("XDG_CURRENT_DESKTOP", desktop);
    QVERIFY(!registered);
    QCOMPARE(readBytes(mimeappsPath()), UserMimeapps);
    QCOMPARE(readBytes(ourDesktopFile()), oldDesktop);
}

void AvdHandlerRegistrationTest::reportsFailedRestore()
{
    if (geteuid() == 0) {
        QSKIP("root writes everywhere");
    }
    QTemporaryDir dir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/restore-XXXXXX"));
    QVERIFY(dir.isValid());
    const QString changed = dir.filePath(QStringLiteral("changed"));
    const QString added = dir.filePath(QStringLiteral("added"));
    const QString missing = dir.filePath(QStringLiteral("missing"));

    const auto before = detail::snapshotFile(changed);
    QCOMPARE(before.kind, detail::FileSnapshot::Kind::Absent);
    writeBytes(changed, "original");
    const auto original = detail::snapshotFile(changed);
    QCOMPARE(original.kind, detail::FileSnapshot::Kind::Present);
    QVERIFY(setMode(changed, 0));
    QCOMPARE(detail::snapshotFile(changed).kind, detail::FileSnapshot::Kind::Unreadable);
    QVERIFY(setMode(changed, 0600));
    writeBytes(changed, "modified");
    writeBytes(added, "new");

    // A read-only folder: neither can be put back, and that is reported.
    QVERIFY(setMode(dir.path(), 0500));
    const bool restoredChanged = detail::restoreFile(changed, original);
    const bool removedAdded = detail::restoreFile(added, before);
    const bool unchangedMissing = detail::restoreFile(missing, before);
    QVERIFY(setMode(dir.path(), 0700));
    QVERIFY(!restoredChanged);
    QVERIFY(!removedAdded);
    QVERIFY(unchangedMissing);

    QVERIFY(detail::restoreFile(changed, original));
    QCOMPARE(readBytes(changed), QByteArray("original"));
    QVERIFY(detail::restoreFile(added, before));
    QVERIFY(!QFile::exists(added));
}

QTEST_GUILESS_MAIN(AvdHandlerRegistrationTest)

#include "avdhandlerregistrationtest.moc"
