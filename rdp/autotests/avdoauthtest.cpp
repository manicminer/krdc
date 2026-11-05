/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdoauth.h"
#include "fakenetwork.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QUrlQuery>

using namespace Avd;

namespace
{
QByteArray unsignedJwt(const QJsonObject &claims)
{
    const auto part = [](const QByteArray &data) {
        return data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    };
    return part(R"({"alg":"none"})") + '.' + part(QJsonDocument(claims).toJson(QJsonDocument::Compact)) + '.';
}

AuthorizationCallback callback(const QString &code, const QString &state)
{
    AuthorizationCallback result;
    result.code = code;
    result.encodedCode = code.toUtf8();
    result.state = state;
    return result;
}
}

class AvdOAuthTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void buildsPkceRequest();
    void requestsAreUnique();
    void extractsCode();
    void refusesForeignOrForgedRedirects_data();
    void refusesForeignOrForgedRedirects();
    void reportsAuthorityError();
    void parsesTokenResponse();
    void refusesBadTokenResponses();
    void redeemsCode();
    void refreshes();
    void reportsTokenErrorWithoutSecrets();
};

void AvdOAuthTest::buildsPkceRequest()
{
    const CloudEndpoints &cloud = publicCloud();
    const AuthorizationRequest request = createAuthorizationRequest(cloud, QStringLiteral("alice@example.org"));

    QCOMPARE(request.url.scheme(), QStringLiteral("https"));
    QCOMPARE(request.url.host(), cloud.authorityHost);
    QCOMPARE(request.url.path(), QStringLiteral("/organizations/oauth2/v2.0/authorize"));

    const QUrlQuery query(request.url);
    QCOMPARE(query.queryItemValue(QStringLiteral("client_id")), cloud.clientId);
    QCOMPARE(query.queryItemValue(QStringLiteral("response_type")), QStringLiteral("code"));
    QCOMPARE(query.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded), brokerRedirectUri(cloud.clientId));
    const QString scope = query.queryItemValue(QStringLiteral("scope"), QUrl::FullyDecoded);
    QVERIFY(scope.contains(QStringLiteral("https://www.wvd.microsoft.com/.default")));
    QVERIFY(scope.contains(QStringLiteral("offline_access")));
    QCOMPARE(query.queryItemValue(QStringLiteral("login_hint"), QUrl::FullyDecoded), QStringLiteral("alice@example.org"));
    QCOMPARE(query.queryItemValue(QStringLiteral("state")), request.state);
    QCOMPARE(query.queryItemValue(QStringLiteral("code_challenge_method")), QStringLiteral("S256"));

    QVERIFY(request.codeVerifier.size() >= 43);
    const QByteArray challenge = QCryptographicHash::hash(request.codeVerifier.toLatin1(), QCryptographicHash::Sha256)
                                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    QCOMPARE(query.queryItemValue(QStringLiteral("code_challenge")).toLatin1(), challenge);
    QVERIFY(!request.url.toString().contains(request.codeVerifier));
}

void AvdOAuthTest::requestsAreUnique()
{
    const auto a = createAuthorizationRequest(publicCloud(), QString());
    const auto b = createAuthorizationRequest(publicCloud(), QString());
    QVERIFY(a.state != b.state);
    QVERIFY(a.codeVerifier != b.codeVerifier);
    QVERIFY(!QUrlQuery(a.url).hasQueryItem(QStringLiteral("login_hint")));
}

void AvdOAuthTest::extractsCode()
{
    const auto request = createAuthorizationRequest(publicCloud(), QString());
    QString error;
    const auto code = authorizationCodeFromCallback(callback(QStringLiteral("the-code"), request.state), request, &error);
    QVERIFY2(code, qPrintable(error));
    QCOMPARE(*code, QStringLiteral("the-code"));
}

void AvdOAuthTest::refusesForeignOrForgedRedirects_data()
{
    QTest::addColumn<QString>("code");
    QTest::addColumn<QString>("state");
    QTest::newRow("state") << QStringLiteral("c") << QStringLiteral("forged");
    QTest::newRow("no state") << QStringLiteral("c") << QString();
    QTest::newRow("no code") << QString() << QStringLiteral("%1");
}

void AvdOAuthTest::refusesForeignOrForgedRedirects()
{
    QFETCH(QString, code);
    QFETCH(QString, state);
    const auto request = createAuthorizationRequest(publicCloud(), QString());
    QString error;
    QVERIFY(!authorizationCodeFromCallback(callback(code, state == QLatin1String("%1") ? request.state : state), request, &error));
    QVERIFY(!error.isEmpty());
    // An empty state never matches, not even an empty one.
    AuthorizationRequest stateless = request;
    stateless.state.clear();
    QVERIFY(!authorizationCodeFromCallback(callback(QStringLiteral("c"), QString()), stateless, &error));
}

void AvdOAuthTest::reportsAuthorityError()
{
    const auto request = createAuthorizationRequest(publicCloud(), QString());
    QString error;
    AuthorizationCallback refused = callback(QString(), request.state);
    refused.error = QStringLiteral("access_denied");
    refused.errorDescription = QStringLiteral("AADSTS53003: Blocked by Conditional Access");
    QVERIFY(!authorizationCodeFromCallback(refused, request, &error));
    QVERIFY2(error.contains(QStringLiteral("AADSTS53003")), qPrintable(error));
}

void AvdOAuthTest::parsesTokenResponse()
{
    QJsonObject body{
        {QStringLiteral("access_token"), QStringLiteral("at")},
        {QStringLiteral("refresh_token"), QStringLiteral("rt")},
        {QStringLiteral("expires_in"), 3599},
        {QStringLiteral("id_token"), QString::fromLatin1(unsignedJwt({{QStringLiteral("preferred_username"), QStringLiteral("alice@example.org")}}))},
    };
    QString error;
    const auto token = parseTokenResponse(QJsonDocument(body).toJson(), &error);
    QVERIFY2(token, qPrintable(error));
    QCOMPARE(token->accessToken, QStringLiteral("at"));
    QCOMPARE(token->refreshToken, QStringLiteral("rt"));
    QCOMPARE(token->expiresIn, 3599);
    QCOMPARE(token->account, QStringLiteral("alice@example.org"));
}

void AvdOAuthTest::refusesBadTokenResponses()
{
    QString error;
    QVERIFY(!parseTokenResponse("not json", &error));
    QVERIFY(!parseTokenResponse(R"({"token_type":"Bearer"})", &error));
    QVERIFY(!parseTokenResponse(QByteArray(MaxTokenResponseSize + 1, ' '), &error));
}

void AvdOAuthTest::redeemsCode()
{
    FakeNetwork network;
    const CloudEndpoints &cloud = publicCloud();
    network.route(cloud.tokenUrl(), 200, R"({"access_token":"at","refresh_token":"rt","expires_in":3600})");
    TokenClient client(&network, cloud);
    QSignalSpy received(&client, &TokenClient::tokenReceived);

    const auto request = createAuthorizationRequest(cloud, QString());
    client.redeemCode(QStringLiteral("the-code"), request);
    QVERIFY(received.wait());
    QCOMPARE(received.first().at(0).value<TokenResponse>().accessToken, QStringLiteral("at"));

    QCOMPARE(network.requests.size(), 1);
    const auto &recorded = network.requests.first();
    QCOMPARE(recorded.request.url(), cloud.tokenUrl());
    QCOMPARE(recorded.request.header(QNetworkRequest::ContentTypeHeader).toString(), QStringLiteral("application/x-www-form-urlencoded"));
    const QUrlQuery form(QString::fromUtf8(recorded.body));
    QCOMPARE(form.queryItemValue(QStringLiteral("grant_type")), QStringLiteral("authorization_code"));
    QCOMPARE(form.queryItemValue(QStringLiteral("code")), QStringLiteral("the-code"));
    QCOMPARE(form.queryItemValue(QStringLiteral("code_verifier")), request.codeVerifier);
    QCOMPARE(form.queryItemValue(QStringLiteral("client_id")), cloud.clientId);
    QCOMPARE(form.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded), brokerRedirectUri(cloud.clientId));
}

void AvdOAuthTest::refreshes()
{
    FakeNetwork network;
    const CloudEndpoints &cloud = publicCloud();
    network.route(cloud.tokenUrl(), 200, R"({"access_token":"at2","expires_in":3600})");
    TokenClient client(&network, cloud);
    QSignalSpy received(&client, &TokenClient::tokenReceived);

    client.refresh(QStringLiteral("old-rt"));
    QVERIFY(received.wait());
    const QUrlQuery form(QString::fromUtf8(network.requests.first().body));
    QCOMPARE(form.queryItemValue(QStringLiteral("grant_type")), QStringLiteral("refresh_token"));
    QCOMPARE(form.queryItemValue(QStringLiteral("refresh_token")), QStringLiteral("old-rt"));
    QVERIFY(form.queryItemValue(QStringLiteral("scope"), QUrl::FullyDecoded).contains(QStringLiteral("offline_access")));
}

void AvdOAuthTest::reportsTokenErrorWithoutSecrets()
{
    FakeNetwork network;
    const CloudEndpoints &cloud = publicCloud();
    network.route(cloud.tokenUrl(), 400, R"({"error":"invalid_grant","error_description":"AADSTS70008: The refresh token has expired."})");
    TokenClient client(&network, cloud);
    QSignalSpy failed(&client, &TokenClient::failed);

    client.refresh(QStringLiteral("secret-refresh-token"));
    QVERIFY(failed.wait());
    const QString message = failed.first().at(0).toString();
    QVERIFY2(message.contains(QStringLiteral("AADSTS70008")), qPrintable(message));
    QVERIFY(!message.contains(QStringLiteral("secret-refresh-token")));
}

QTEST_GUILESS_MAIN(AvdOAuthTest)

#include "avdoauthtest.moc"
