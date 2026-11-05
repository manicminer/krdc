/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "rdpcredentials.h"

#include <QTest>

using Target = RdpCredentialRouter::Target;

class RdpCredentialsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void mapsReasons_data();
    void mapsReasons();
    void gatewayPromptDoesNotUseHostIdentity();
    void gatewayAnswerLeavesHostAlone();
    void gatewaySharesHostLoginWhenConfigured();
    void importedUserNameIsEditableSuggestion();
    void answersAutomaticallyOnceWithPassword();
    void doesNotAnswerAutomaticallyWithoutPassword();
    void flagsUnexpectedPasswordLogon();
    void smartcardPinAsksOnlyForPin();
};

void RdpCredentialsTest::mapsReasons_data()
{
    QTest::addColumn<int>("reason");
    QTest::addColumn<int>("target");

    QTest::newRow("nla") << int(AUTH_NLA) << int(Target::Host);
    QTest::newRow("tls") << int(AUTH_TLS) << int(Target::Host);
    QTest::newRow("rdp") << int(AUTH_RDP) << int(Target::Host);
    QTest::newRow("rdstls") << int(AUTH_RDSTLS) << int(Target::Host);
    QTest::newRow("gw http") << int(GW_AUTH_HTTP) << int(Target::Gateway);
    QTest::newRow("gw rdg") << int(GW_AUTH_RDG) << int(Target::Gateway);
    QTest::newRow("gw rpc") << int(GW_AUTH_RPC) << int(Target::Gateway);
    QTest::newRow("pin") << int(AUTH_SMARTCARD_PIN) << int(Target::SmartcardPin);
    QTest::newRow("fido") << int(AUTH_FIDO_PIN) << int(Target::Unsupported);
}

void RdpCredentialsTest::mapsReasons()
{
    QFETCH(int, reason);
    QFETCH(int, target);
    QCOMPARE(int(RdpCredentialRouter::targetForReason(static_cast<rdp_auth_reason>(reason))), target);
}

void RdpCredentialsTest::gatewayPromptDoesNotUseHostIdentity()
{
    RdpCredentialRouter router;
    router.setHostCredentials({QStringLiteral("alice@example.org"), QString(), QString()});
    router.setGatewayCredentials({QStringLiteral("gw-user"), QStringLiteral("GW"), QString()});

    const auto prompt = router.promptFor(Target::Gateway);
    QCOMPARE(prompt.target, Target::Gateway);
    QCOMPARE(prompt.username, QStringLiteral("gw-user"));
    QCOMPARE(prompt.domain, QStringLiteral("GW"));
    QVERIFY(prompt.usernameEditable);

    RdpCredentialRouter empty;
    empty.setHostCredentials({QStringLiteral("alice@example.org"), QStringLiteral("EXAMPLE"), QStringLiteral("secret")});
    const auto gatewayPrompt = empty.promptFor(Target::Gateway);
    QVERIFY(gatewayPrompt.username.isEmpty());
    QVERIFY(gatewayPrompt.domain.isEmpty());
    QVERIFY(!empty.automaticAnswer(Target::Gateway));
}

void RdpCredentialsTest::gatewayAnswerLeavesHostAlone()
{
    RdpCredentialRouter router;
    router.setHostCredentials({QStringLiteral("alice@example.org"), QString(), QStringLiteral("host-secret")});
    router.accept(Target::Gateway, {QStringLiteral("gw-user"), QStringLiteral("GW"), QStringLiteral("gw-secret")});

    QCOMPARE(router.hostCredentials().username, QStringLiteral("alice@example.org"));
    QCOMPARE(router.hostCredentials().password, QStringLiteral("host-secret"));
    QCOMPARE(router.gatewayCredentials().username, QStringLiteral("gw-user"));

    router.accept(Target::Host, {QStringLiteral("bob@example.org"), QString(), QStringLiteral("other")});
    QCOMPARE(router.gatewayCredentials().username, QStringLiteral("gw-user"));
    QCOMPARE(router.gatewayCredentials().password, QStringLiteral("gw-secret"));
}

void RdpCredentialsTest::gatewaySharesHostLoginWhenConfigured()
{
    RdpCredentialRouter router;
    router.setGatewayUsesHostCredentials(true);
    router.setHostCredentials({QStringLiteral("alice"), QStringLiteral("EXAMPLE"), QStringLiteral("secret")});

    const auto answer = router.automaticAnswer(Target::Gateway);
    QVERIFY(answer);
    QCOMPARE(answer->username, QStringLiteral("alice"));
    QCOMPARE(answer->domain, QStringLiteral("EXAMPLE"));
}

void RdpCredentialsTest::importedUserNameIsEditableSuggestion()
{
    RdpCredentialRouter router;
    router.setHostCredentials({QStringLiteral("imported@example.org"), QString(), QString()});

    const auto prompt = router.promptFor(Target::Host);
    QCOMPARE(prompt.username, QStringLiteral("imported@example.org"));
    QVERIFY(prompt.usernameEditable);
    QVERIFY(prompt.showUsername);

    router.accept(Target::Host, {QStringLiteral("corrected@example.org"), QString(), QStringLiteral("pw")});
    QCOMPARE(router.promptFor(Target::Host).username, QStringLiteral("corrected@example.org"));
}

void RdpCredentialsTest::answersAutomaticallyOnceWithPassword()
{
    RdpCredentialRouter router;
    router.setHostCredentials({QStringLiteral("alice"), QStringLiteral("EXAMPLE"), QStringLiteral("secret")});

    const auto first = router.automaticAnswer(Target::Host);
    QVERIFY(first);
    QCOMPARE(first->password, QStringLiteral("secret"));
    // A rejected password must lead to a prompt, not a loop.
    QVERIFY(!router.automaticAnswer(Target::Host));
}

void RdpCredentialsTest::doesNotAnswerAutomaticallyWithoutPassword()
{
    RdpCredentialRouter router;
    router.setHostCredentials({QStringLiteral("alice"), QString(), QString()});
    QVERIFY(!router.automaticAnswer(Target::Host));
}

void RdpCredentialsTest::flagsUnexpectedPasswordLogon()
{
    RdpCredentialRouter router;
    router.setEntraExpected(true);
    QVERIFY(router.promptFor(Target::Host).entraExpected);
    QVERIFY(!router.promptFor(Target::Gateway).entraExpected);
}

void RdpCredentialsTest::smartcardPinAsksOnlyForPin()
{
    RdpCredentialRouter router;
    router.setHostCredentials({QStringLiteral("alice"), QStringLiteral("EXAMPLE"), QString()});
    const auto prompt = router.promptFor(Target::SmartcardPin);
    QVERIFY(!prompt.showUsername);
    QVERIFY(!prompt.showDomain);
}

QTEST_GUILESS_MAIN(RdpCredentialsTest)

#include "rdpcredentialstest.moc"
