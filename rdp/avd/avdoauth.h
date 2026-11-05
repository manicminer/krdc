/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include "avdcallback.h"
#include "avdendpoints.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace Avd
{

/** An authorization code request with PKCE (RFC 7636) for the workspace feed. */
struct AuthorizationRequest {
    QUrl url;
    /** Where the browser hands the answer to, see brokerRedirectUri(). */
    QString redirectUri;
    QString state;
    QString codeVerifier;
};

AuthorizationRequest createAuthorizationRequest(const CloudEndpoints &endpoints, const QString &loginHint);

/**
 * Takes the authorization code from the answer to @p request. Refuses a
 * mismatched state; reports errors sent by the authority, which include the
 * reasons of conditional access policies.
 */
std::optional<QString> authorizationCodeFromCallback(const AuthorizationCallback &callback, const AuthorizationRequest &request, QString *errorMessage);

struct TokenResponse {
    QString accessToken;
    QString refreshToken;
    qint64 expiresIn = 0;
    /** Account name from the ID token, used to label the account. */
    QString account;
};

/** Largest token response KRDC parses. */
constexpr qsizetype MaxTokenResponseSize = 256 * 1024;

std::optional<TokenResponse> parseTokenResponse(const QByteArray &json, QString *errorMessage);

/** Redeems codes and refresh tokens at the token endpoint. */
class TokenClient : public QObject
{
    Q_OBJECT

public:
    TokenClient(QNetworkAccessManager *network, const CloudEndpoints &endpoints, QObject *parent = nullptr);

    void redeemCode(const QString &code, const AuthorizationRequest &request);
    void refresh(const QString &refreshToken);
    void abort();

Q_SIGNALS:
    void tokenReceived(const Avd::TokenResponse &response);
    void failed(const QString &message);

private:
    void post(const QByteArray &body);

    QNetworkAccessManager *m_network;
    CloudEndpoints m_endpoints;
    QPointer<QNetworkReply> m_reply;
};

} // namespace Avd

Q_DECLARE_METATYPE(Avd::TokenResponse)
