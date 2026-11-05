/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "connectionlaunch.h"
#include "remoteviewfactory.h"

#include <QTest>

namespace
{
class UrlOnlyFactory : public RemoteViewFactory
{
public:
    bool supportsUrl(const QUrl &url) const override
    {
        return url.scheme() == QLatin1String("test");
    }
    ConnectionFile loadConnectionFile(const QUrl &url) const override
    {
        if (!url.path().endsWith(QLatin1String(".test"))) {
            return {};
        }
        return {QUrl(QStringLiteral("test://host.example.org:1234")), {{QStringLiteral("key"), QStringLiteral("value")}}};
    }
    RemoteView *createView(QWidget *, const QUrl &url, KConfigGroup) override
    {
        lastUrl = url;
        return nullptr;
    }
    HostPreferences *createHostPreferences(KConfigGroup, QWidget *) override
    {
        return nullptr;
    }
    QString scheme() const override
    {
        return QStringLiteral("test");
    }
    QString connectActionText() const override
    {
        return {};
    }
    QString connectButtonText() const override
    {
        return {};
    }
    QString connectToolTipText() const override
    {
        return {};
    }

    mutable QUrl lastUrl;
};

class Payload : public LaunchPayload
{
public:
    QString kind() const override
    {
        return QStringLiteral("test");
    }
};
}

class ConnectionLaunchTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void fromUrlIsRemembered();
    void defaultLoadWrapsUrl();
    void defaultLoadIgnoresOtherFiles();
    void defaultCreateViewUsesUrl();
    void defaultHasNoActions();
};

void ConnectionLaunchTest::fromUrlIsRemembered()
{
    const auto launch = ConnectionLaunch::fromUrl(QUrl(QStringLiteral("test://h.example.org")));
    QVERIFY(launch.isValid());
    QVERIFY(launch.remember);
    QVERIFY(!launch.payload);
    QVERIFY(!launch.fromFile);
    QVERIFY(!ConnectionLaunch().isValid());
}

void ConnectionLaunchTest::defaultLoadWrapsUrl()
{
    UrlOnlyFactory factory;
    QString error;
    const auto launch = factory.loadLaunchFromFile(QUrl::fromLocalFile(QStringLiteral("/tmp/x.test")), &error);
    QVERIFY(launch.isValid());
    QCOMPARE(launch.url, QUrl(QStringLiteral("test://host.example.org:1234")));
    QVERIFY(launch.remember);
    QVERIFY(launch.fromFile);
    QCOMPARE(launch.fileSettings.value(QStringLiteral("key")), QStringLiteral("value"));
    QVERIFY(error.isEmpty());
}

void ConnectionLaunchTest::defaultLoadIgnoresOtherFiles()
{
    UrlOnlyFactory factory;
    QString error;
    QVERIFY(!factory.loadLaunchFromFile(QUrl::fromLocalFile(QStringLiteral("/tmp/x.other")), &error).isValid());
    QVERIFY(error.isEmpty());
}

void ConnectionLaunchTest::defaultCreateViewUsesUrl()
{
    UrlOnlyFactory factory;
    ConnectionLaunch launch = ConnectionLaunch::fromUrl(QUrl(QStringLiteral("test://h.example.org")));
    launch.payload = std::make_shared<Payload>();
    factory.createViewForLaunch(nullptr, launch, KConfigGroup());
    QCOMPARE(factory.lastUrl, launch.url);
}

void ConnectionLaunchTest::defaultHasNoActions()
{
    UrlOnlyFactory factory;
    QVERIFY(factory.createActions(nullptr).isEmpty());
}

QTEST_GUILESS_MAIN(ConnectionLaunchTest)

#include "connectionlaunchtest.moc"
