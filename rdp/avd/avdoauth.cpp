/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdoauth.h"

#include <KLocalizedString>

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QUrlQuery>

namespace Avd
{

namespace
{
constexpr int TransferTimeout = 30000;

QString randomUrlSafe(int bytes)
{
    QByteArray data(bytes, Qt::Uninitialized);
    QRandomGenerator::system()->generate(data.begin(), data.end());
    return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray formEncode(const QList<std::pair<QString, QString>> &items)
{
    QByteArray body;
    for (const auto &[key, value] : items) {
        if (!body.isEmpty()) {
            body += '&';
        }
        body += QUrl::toPercentEncoding(key) + '=' + QUrl::toPercentEncoding(value);
    }
    return body;
}

QString accountFromIdToken(const QString &idToken)
{
    const QStringList parts = idToken.split(QLatin1Char('.'));
    if (parts.size() < 2) {
        return {};
    }
    // Only a label for the account: the token is not verified, and nothing is decided on it.
    const QByteArray payload = QByteArray::fromBase64(parts.at(1).toLatin1(), QByteArray::Base64UrlEncoding);
    const QJsonObject claims = QJsonDocument::fromJson(payload).object();
    for (const char *claim : {"preferred_username", "upn", "email"}) {
        const QString value = claims.value(QLatin1String(claim)).toString();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}
}

AuthorizationRequest createAuthorizationRequest(const CloudEndpoints &endpoints, const QString &loginHint)
{
    AuthorizationRequest request;
    request.redirectUri = brokerRedirectUri(endpoints.clientId);
    request.state = randomUrlSafe(32);
    request.codeVerifier = randomUrlSafe(48);
    const QByteArray challenge = QCryptographicHash::hash(request.codeVerifier.toLatin1(), QCryptographicHash::Sha256)
                                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client_id"), endpoints.clientId);
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("redirect_uri"), QString::fromLatin1(QUrl::toPercentEncoding(request.redirectUri)));
    query.addQueryItem(QStringLiteral("scope"), QString::fromLatin1(QUrl::toPercentEncoding(endpoints.feedScopes.join(QLatin1Char(' ')))));
    query.addQueryItem(QStringLiteral("state"), request.state);
    query.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(challenge));
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    query.addQueryItem(QStringLiteral("prompt"), QStringLiteral("select_account"));
    if (!loginHint.isEmpty()) {
        query.addQueryItem(QStringLiteral("login_hint"), QString::fromLatin1(QUrl::toPercentEncoding(loginHint)));
    }

    request.url = endpoints.authorizeUrl();
    request.url.setQuery(query);
    return request;
}

std::optional<QString> authorizationCodeFromCallback(const AuthorizationCallback &callback, const AuthorizationRequest &request, QString *errorMessage)
{
    const auto fail = [errorMessage](const QString &message) -> std::optional<QString> {
        if (errorMessage) {
            *errorMessage = message;
        }
        return std::nullopt;
    };
    if (request.state.isEmpty() || callback.state != request.state) {
        return fail(i18nc("@info", "The sign-in answer does not belong to this sign-in. Sign in again."));
    }
    if (callback.isError()) {
        return fail(i18nc("@info",
                          "Microsoft Entra ID did not allow the sign-in: %1",
                          callback.errorDescription.isEmpty() ? callback.error : callback.errorDescription));
    }
    if (callback.code.isEmpty()) {
        return fail(i18nc("@info", "The sign-in answer does not contain an authorization code."));
    }
    return callback.code;
}

std::optional<TokenResponse> parseTokenResponse(const QByteArray &json, QString *errorMessage)
{
    const auto fail = [errorMessage](const QString &message) -> std::optional<TokenResponse> {
        if (errorMessage) {
            *errorMessage = message;
        }
        return std::nullopt;
    };
    if (json.size() > MaxTokenResponseSize) {
        return fail(i18nc("@info", "The sign-in service sent more data than KRDC accepts."));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(i18nc("@info", "The sign-in service sent an answer KRDC cannot read."));
    }
    const QJsonObject object = document.object();
    if (object.contains(QLatin1String("error"))) {
        const QString description = object.value(QLatin1String("error_description")).toString();
        return fail(i18nc("@info",
                          "Microsoft Entra ID refused the sign-in: %1",
                          (description.isEmpty() ? object.value(QLatin1String("error")).toString() : description).left(500)));
    }
    TokenResponse response;
    response.accessToken = object.value(QLatin1String("access_token")).toString();
    response.refreshToken = object.value(QLatin1String("refresh_token")).toString();
    response.expiresIn = object.value(QLatin1String("expires_in")).toInteger();
    response.account = accountFromIdToken(object.value(QLatin1String("id_token")).toString());
    if (response.accessToken.isEmpty()) {
        return fail(i18nc("@info", "The sign-in service did not issue an access token."));
    }
    return response;
}

TokenClient::TokenClient(QNetworkAccessManager *network, const CloudEndpoints &endpoints, QObject *parent)
    : QObject(parent)
    , m_network(network)
    , m_endpoints(endpoints)
{
}

void TokenClient::redeemCode(const QString &code, const AuthorizationRequest &request)
{
    post(formEncode({
        {QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
        {QStringLiteral("client_id"), m_endpoints.clientId},
        {QStringLiteral("code"), code},
        {QStringLiteral("code_verifier"), request.codeVerifier},
        {QStringLiteral("redirect_uri"), request.redirectUri},
        {QStringLiteral("scope"), m_endpoints.feedScopes.join(QLatin1Char(' '))},
    }));
}

void TokenClient::refresh(const QString &refreshToken)
{
    post(formEncode({
        {QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
        {QStringLiteral("client_id"), m_endpoints.clientId},
        {QStringLiteral("refresh_token"), refreshToken},
        {QStringLiteral("scope"), m_endpoints.feedScopes.join(QLatin1Char(' '))},
    }));
}

void TokenClient::abort()
{
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->abort();
    }
}

void TokenClient::post(const QByteArray &body)
{
    abort();
    const QUrl url = m_endpoints.tokenUrl();
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(TransferTimeout);

    QNetworkReply *reply = m_network->post(request, body);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_reply != reply) {
            return;
        }
        m_reply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 0) {
            Q_EMIT failed(i18nc("@info", "The sign-in service could not be reached: %1", reply->errorString()));
            return;
        }
        const QByteArray data = reply->read(MaxTokenResponseSize + 1);
        QString error;
        const auto response = parseTokenResponse(data, &error);
        if (!response || status != 200) {
            Q_EMIT failed(error.isEmpty() ? i18nc("@info", "The sign-in service answered with HTTP status %1.", status) : error);
            return;
        }
        Q_EMIT tokenReceived(*response);
    });
}

} // namespace Avd
