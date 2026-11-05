/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "rdpaccesstoken.h"
#include "avd/avdcallback.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QTest>
#include <QUrlQuery>

#include <freerdp/client.h>
#include <winpr/crt.h>

namespace
{
const QByteArray HostScope = "ms-device-service%3A%2F%2Ftermsrv.wvd.microsoft.com%2Fname%2Fhost.example.org%2Fuser_impersonation";
const QByteArray ReqCnf = "synthetic-req-cnf";
const QString Authority = QStringLiteral("login.microsoftonline.com");
const QString State = QStringLiteral("73796e7468657469632d73746174652d666f722d6b7264632d7465737473");
const QString ClientId = QStringLiteral("a85cf173-4192-42f8-81fa-777a763e6e2c");
// As FreeRDP encodes its redirect URI, see FreeRDP_GatewayAvdAccessTokenFormat.
const QByteArray EncodedRedirect = "ms-appx-web%3a%2f%2fMicrosoft.AAD.BrokerPlugin%2fa85cf173-4192-42f8-81fa-777a763e6e2c";
const QByteArray Pkce = "&code_challenge=synthetic-challenge&code_challenge_method=S256";

bool request(RdpAccessTokenBroker &broker, AccessTokenType type, char **token, size_t count, ...)
{
    va_list ap;
    va_start(ap, count);
    const bool rc = broker.getAccessToken(type, token, count, ap);
    va_end(ap);
    return rc;
}

struct Context {
    Context()
    {
        RDP_CLIENT_ENTRY_POINTS entry = {};
        entry.Version = RDP_CLIENT_INTERFACE_VERSION;
        entry.Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
        entry.ContextSize = sizeof(rdpClientContext);
        context = freerdp_client_context_new(&entry);
        // AVD tokens only go to an AVD gateway; tests that need another one set it.
        setGateway("rdgateway.wvd.microsoft.com", 443);
    }
    void setGateway(const char *host, UINT32 port)
    {
        (void)freerdp_settings_set_string(context->settings, FreeRDP_GatewayHostname, host);
        (void)freerdp_settings_set_uint32(context->settings, FreeRDP_GatewayPort, port);
    }
    ~Context()
    {
        freerdp_client_context_free(context);
    }
    rdpClientContext *client() const
    {
        return reinterpret_cast<rdpClientContext *>(context);
    }
    rdpContext *context = nullptr;
};

// Stands in for FreeRDP's request building, which needs the authority's
// discovery document from the network.
struct FakeOAuth {
    QList<freerdp_client_aad_type> types;
    QList<QByteArray> scopes;
    QList<QByteArray> codes;
    QList<QByteArray> reqCnfs;
    QList<bool> stdioDuringBuild;
    QList<QByteArray> exchanged;
    QList<QByteArray> extracted;
    QByteArray clientIdQuery = "client_id=" + ClientId.toUtf8();
    QByteArray authorizeQuery = "&redirect_uri=" + EncodedRedirect + "&state=" + State.toUtf8() + Pkce;
    rdpSettings *settings = nullptr;

    void install(RdpAccessTokenBroker &broker)
    {
        broker.setRequestBuilder([this](freerdp_client_aad_type type, const QByteArray &scope, const QByteArray &code, const QByteArray &reqCnf) {
            types << type;
            scopes << scope;
            codes << code;
            reqCnfs << reqCnf;
            stdioDuringBuild << freerdp_settings_get_bool(settings, FreeRDP_UseCommonStdioCallbacks);
            switch (type) {
            case FREERDP_CLIENT_AAD_AUTH_REQUEST:
            case FREERDP_CLIENT_AAD_AVD_AUTH_REQUEST:
                return QByteArray("https://login.microsoftonline.com/common/oauth2/v2.0/authorize?" + clientIdQuery + "&response_type=code&scope=" + scope
                                  + authorizeQuery);
            default:
                return QByteArray("grant_type=authorization_code&code=" + code + "&req_cnf=" + reqCnf);
            }
        });
        // Like FreeRDP's: finds the first "state=" and "code=".
        broker.setCodeExtractor([this](const QByteArray &response) {
            extracted << response;
            const QUrlQuery query(QString::fromUtf8(response));
            if (query.queryItemValue(QStringLiteral("state")) != State) {
                return QByteArray();
            }
            return query.queryItemValue(QStringLiteral("code"), QUrl::FullyEncoded).toUtf8();
        });
        broker.setTokenExchange([this](const QByteArray &tokenRequest, char **token) {
            exchanged << tokenRequest;
            *token = strdup("synthetic-access-token");
            return true;
        });
    }
};

QByteArray takeExtracted(rdpClientContext *context, const QByteArray &response)
{
    char *code = freerdp_client_extract_aad_code(context, response.constData(), static_cast<size_t>(response.size()));
    const QByteArray result(code);
    winpr_zfree(code);
    return result;
}

RdpAccessTokenBroker::Helper unavailableHelper()
{
    return [](AccessTokenType, char **, size_t, va_list) {
        return RdpAccessTokenBroker::HelperResult::Unavailable;
    };
}

std::optional<Avd::AuthorizationCallback> answer(const QString &query)
{
    QString error;
    const auto callback = Avd::parseAuthorizationCallback((Avd::brokerRedirectUri(ClientId) + QLatin1Char('?') + query).toUtf8(),
                                                          Avd::brokerRedirectUri(ClientId),
                                                          State,
                                                          &error);
    if (!callback) {
        qWarning() << error;
    }
    return callback;
}
}

class RdpAccessTokenTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void avdGatewayTokenThroughBrowser();
    void remoteComputerTokenKeepsScopeAndProofOfPossession();
    void freerdpDefaultRedirectIsBrokerRedirect();
    void usesBrokerRedirectAndRestoresStdioSetting();
    void passesOnlyCheckedAnswerToFreeRdp();
    void refusesStateMismatch();
    void refusesUnsafeAuthorizeRequest_data();
    void refusesUnsafeAuthorizeRequest();
    void refusesWrongClientId_data();
    void refusesWrongClientId();
    void reportsAuthorityError();
    void diagnosticHasNoCallbackText();
    void realFreeRdpAvdRequestsCarryPkce();
    void cancelIsNotAnError();
    void helperTokenSkipsBrowser();
    void failedHelperIsNotRetriedInBrowser();
    void rejectsMissingArguments();
    void freerdpHelperReportsUnavailable();
    void autodetectedHelperIsNotStarted();
    void helperThatExitsDoesNotKillClient();
    void avdTokenOnlyForTrustedGateway_data();
    void avdTokenOnlyForTrustedGateway();
};

void RdpAccessTokenTest::avdGatewayTokenThroughBrowser()
{
    Context ctx;
    QVERIFY(ctx.context);
    RdpAccessTokenBroker broker(ctx.client());
    FakeOAuth oauth;
    oauth.settings = ctx.context->settings;
    oauth.install(broker);
    broker.setHelper(unavailableHelper());

    QList<RdpAccessTokenBroker::SignInRequest> prompts;
    broker.setSignInPrompt([&prompts](const RdpAccessTokenBroker::SignInRequest &request) -> std::optional<Avd::AuthorizationCallback> {
        prompts << request;
        return answer(QStringLiteral("code=abc&state=") + State);
    });

    char *token = nullptr;
    QVERIFY(request(broker, ACCESS_TOKEN_TYPE_AVD, &token, 0));
    QCOMPARE(QByteArray(token), QByteArray("synthetic-access-token"));
    free(token);

    QCOMPARE(prompts.size(), 1);
    QCOMPARE(prompts.first().kind, RdpAccessTokenBroker::Kind::AvdGateway);
    QCOMPARE(prompts.first().authorityHost, Authority);
    QCOMPARE(prompts.first().authorizeUrl.host(), Authority);
    QCOMPARE(prompts.first().redirectUri, Avd::brokerRedirectUri(ClientId));
    QCOMPARE(prompts.first().state, State);
    QCOMPARE(oauth.types, (QList<freerdp_client_aad_type>{FREERDP_CLIENT_AAD_AVD_AUTH_REQUEST, FREERDP_CLIENT_AAD_AVD_TOKEN_REQUEST}));
    QCOMPARE(oauth.codes.last(), QByteArray("abc"));
    QCOMPARE(oauth.exchanged.size(), 1);
    QVERIFY(!broker.canceled());
}

void RdpAccessTokenTest::remoteComputerTokenKeepsScopeAndProofOfPossession()
{
    Context ctx;
    RdpAccessTokenBroker broker(ctx.client());
    FakeOAuth oauth;
    oauth.settings = ctx.context->settings;
    oauth.install(broker);
    broker.setHelper(unavailableHelper());

    RdpAccessTokenBroker::Kind kind = RdpAccessTokenBroker::Kind::AvdGateway;
    broker.setSignInPrompt([&kind](const RdpAccessTokenBroker::SignInRequest &request) -> std::optional<Avd::AuthorizationCallback> {
        kind = request.kind;
        return answer(QStringLiteral("code=host-code&state=") + State + QStringLiteral("&session_state=x"));
    });

    char *token = nullptr;
    QVERIFY(request(broker, ACCESS_TOKEN_TYPE_AAD, &token, 2, HostScope.constData(), ReqCnf.constData()));
    free(token);

    QCOMPARE(kind, RdpAccessTokenBroker::Kind::RemoteComputer);
    QCOMPARE(oauth.types, (QList<freerdp_client_aad_type>{FREERDP_CLIENT_AAD_AUTH_REQUEST, FREERDP_CLIENT_AAD_TOKEN_REQUEST}));
    QCOMPARE(oauth.scopes, (QList<QByteArray>{HostScope, HostScope}));
    QCOMPARE(oauth.reqCnfs.last(), ReqCnf);
    QCOMPARE(oauth.codes.last(), QByteArray("host-code"));
}

void RdpAccessTokenTest::freerdpDefaultRedirectIsBrokerRedirect()
{
    // KRDC waits for the redirect FreeRDP puts into its requests by default.
    Context ctx;
    QByteArray format(freerdp_settings_get_string(ctx.context->settings, FreeRDP_GatewayAvdAccessTokenFormat));
    const QByteArray clientId(freerdp_settings_get_string(ctx.context->settings, FreeRDP_GatewayAvdClientID));
    QCOMPARE(clientId, ClientId.toUtf8());
    QVERIFY(format.contains("%s"));
    format.replace("%%", "%").replace("%s", clientId);
    QCOMPARE(QUrl::fromPercentEncoding(format), Avd::brokerRedirectUri(ClientId));
}

void RdpAccessTokenTest::usesBrokerRedirectAndRestoresStdioSetting()
{
    Context ctx;
    RdpAccessTokenBroker broker(ctx.client());
    FakeOAuth oauth;
    oauth.settings = ctx.context->settings;
    oauth.install(broker);
    broker.setHelper(unavailableHelper());
    broker.setSignInPrompt([](const RdpAccessTokenBroker::SignInRequest &) -> std::optional<Avd::AuthorizationCallback> {
        return answer(QStringLiteral("code=abc&state=") + State);
    });

    QVERIFY(freerdp_settings_set_bool(ctx.context->settings, FreeRDP_UseCommonStdioCallbacks, TRUE));
    char *token = nullptr;
    QVERIFY(request(broker, ACCESS_TOKEN_TYPE_AVD, &token, 0));
    free(token);
    // FreeRDP only uses the ms-appx-web redirect without its terminal callbacks.
    QCOMPARE(oauth.stdioDuringBuild, (QList<bool>{false, false}));
    QVERIFY(freerdp_settings_get_bool(ctx.context->settings, FreeRDP_UseCommonStdioCallbacks));
}

void RdpAccessTokenTest::passesOnlyCheckedAnswerToFreeRdp()
{
    Context ctx;
    RdpAccessTokenBroker broker(ctx.client());
    FakeOAuth oauth;
    oauth.settings = ctx.context->settings;
    oauth.install(broker);
    broker.setHelper(unavailableHelper());
    broker.setSignInPrompt([](const RdpAccessTokenBroker::SignInRequest &) -> std::optional<Avd::AuthorizationCallback> {
        return answer(QStringLiteral("session_state=xcode%3Dforged&code=a%2Fb&state=") + State);
    });

    char *token = nullptr;
    QVERIFY(request(broker, ACCESS_TOKEN_TYPE_AVD, &token, 0));
    free(token);
    // FreeRDP's own parsing is loose; it only gets the checked state and code.
    QCOMPARE(oauth.extracted, QList<QByteArray>{"state=" + State.toUtf8() + "&code=a%2Fb"});
    QCOMPARE(oauth.codes.last(), QByteArray("a%2Fb"));
}

void RdpAccessTokenTest::refusesStateMismatch()
{
    Context ctx;
    RdpAccessTokenBroker broker(ctx.client());
    FakeOAuth oauth;
    oauth.settings = ctx.context->settings;
    oauth.install(broker);
    broker.setHelper(unavailableHelper());
    broker.setSignInPrompt([](const RdpAccessTokenBroker::SignInRequest &) -> std::optional<Avd::AuthorizationCallback> {
        Avd::AuthorizationCallback callback;
        callback.code = QStringLiteral("abc");
        callback.encodedCode = "abc";
        callback.state = QStringLiteral("forged");
        return callback;
    });

    char *token = nullptr;
    QVERIFY(!request(broker, ACCESS_TOKEN_TYPE_AVD, &token, 0));
    QVERIFY(!token);
    QVERIFY(oauth.exchanged.isEmpty());
    QVERIFY(oauth.extracted.isEmpty());
    QVERIFY(!broker.lastError().isEmpty());
    QVERIFY(!broker.lastError().contains(QStringLiteral("abc")));
}

void RdpAccessTokenTest::refusesUnsafeAuthorizeRequest_data()
{
    QTest::addColumn<QByteArray>("query");
    const QByteArray state = "&state=" + State.toUtf8();
    const QByteArray redirect = "&redirect_uri=" + EncodedRedirect;
    QTest::newRow("nativeclient") << QByteArray("&redirect_uri=https%3A%2F%2Flogin.microsoftonline.com%2Fcommon%2Foauth2%2Fnativeclient" + state + Pkce);
    QTest::newRow("other client") << QByteArray("&redirect_uri=ms-appx-web%3a%2f%2fMicrosoft.AAD.BrokerPlugin%2fother" + state + Pkce);
    QTest::newRow("no redirect") << QByteArray(state + Pkce);
    QTest::newRow("no state") << QByteArray(redirect + Pkce);
    QTest::newRow("two states") << QByteArray(redirect + state + state + Pkce);
    QTest::newRow("no pkce") << QByteArray(redirect + state);
    QTest::newRow("plain pkce") << QByteArray(redirect + state + "&code_challenge=x&code_challenge_method=plain");
}

void RdpAccessTokenTest::refusesUnsafeAuthorizeRequest()
{
    QFETCH(QByteArray, query);
    Context ctx;
    RdpAccessTokenBroker broker(ctx.client());
    FakeOAuth oauth;
    oauth.settings = ctx.context->settings;
    oauth.authorizeQuery = query;
    oauth.install(broker);
    broker.setHelper(unavailableHelper());
    bool prompted = false;
    broker.setSignInPrompt([&prompted](const RdpAccessTokenBroker::SignInRequest &) -> std::optional<Avd::AuthorizationCallback> {
        prompted = true;
        return answer(QStringLiteral("code=abc&state=") + State);
    });

    char *token = nullptr;
    QVERIFY(!request(broker, ACCESS_TOKEN_TYPE_AAD, &token, 2, HostScope.constData(), ReqCnf.constData()));
    QVERIFY(!prompted);
    QVERIFY(oauth.exchanged.isEmpty());
    QVERIFY(!broker.lastError().isEmpty());
}

void RdpAccessTokenTest::reportsAuthorityError()
{
    Context ctx;
    RdpAccessTokenBroker broker(ctx.client());
    FakeOAuth oauth;
    oauth.settings = ctx.context->settings;
    oauth.install(broker);
    broker.setHelper(unavailableHelper());
    broker.setSignInPrompt([](const RdpAccessTokenBroker::SignInRequest &) -> std::optional<Avd::AuthorizationCallback> {
        return answer(QStringLiteral("error=access_denied&error_description=AADSTS53003%3A+Blocked+by+Conditional+Access&state=") + State);
    });

    char *token = nullptr;
    QVERIFY(!request(broker, ACCESS_TOKEN_TYPE_AAD, &token, 2, HostScope.constData(), ReqCnf.constData()));
    QVERIFY(oauth.exchanged.isEmpty());
    QVERIFY(!broker.canceled());
    QVERIFY2(broker.lastError().contains(QStringLiteral("AADSTS53003")), qPrintable(broker.lastError()));
}

void RdpAccessTokenTest::refusesWrongClientId_data()
{
    QTest::addColumn<QByteArray>("clientIdQuery");
    QTest::newRow("other") << QByteArray("client_id=00000000-0000-0000-0000-000000000000");
    QTest::newRow("upper case") << QByteArray("client_id=" + ClientId.toUpper().toUtf8());
    QTest::newRow("prefix") << QByteArray("client_id=" + ClientId.toUtf8() + "0");
    QTest::newRow("twice") << QByteArray("client_id=" + ClientId.toUtf8() + "&client_id=" + ClientId.toUtf8());
    QTest::newRow("missing") << QByteArray("x=y");
}

void RdpAccessTokenTest::refusesWrongClientId()
{
    QFETCH(QByteArray, clientIdQuery);
    Context ctx;
    RdpAccessTokenBroker broker(ctx.client());
    FakeOAuth oauth;
    oauth.settings = ctx.context->settings;
    oauth.clientIdQuery = clientIdQuery;
    oauth.install(broker);
    broker.setHelper(unavailableHelper());
    bool prompted = false;
    broker.setSignInPrompt([&prompted](const RdpAccessTokenBroker::SignInRequest &) -> std::optional<Avd::AuthorizationCallback> {
        prompted = true;
        return answer(QStringLiteral("code=abc&state=") + State);
    });

    char *token = nullptr;
    QVERIFY(!request(broker, ACCESS_TOKEN_TYPE_AVD, &token, 0));
    QVERIFY(!prompted);
    QVERIFY(oauth.exchanged.isEmpty());
}

void RdpAccessTokenTest::diagnosticHasNoCallbackText()
{
    // Whatever the browser hands back may carry links, codes or tokens; logs must not keep them.
    const QStringList secrets{QStringLiteral("SYNTHETICSECRET"), QStringLiteral("leak.example"), QStringLiteral("SYNTHCODE")};
    const QStringList answers{
        QStringLiteral("error=access_denied&error_description=See+https%3A%2F%2Fleak.example%2F%3Fcode%3DSYNTHCODE+token+SYNTHETICSECRET&state=") + State,
        QStringLiteral("error=SYNTHETICSECRET_SYNTHCODE&state=") + State,
    };
    for (const QString &query : answers) {
        Context ctx;
        RdpAccessTokenBroker broker(ctx.client());
        FakeOAuth oauth;
        oauth.settings = ctx.context->settings;
        oauth.install(broker);
        broker.setHelper(unavailableHelper());
        broker.setSignInPrompt([&query](const RdpAccessTokenBroker::SignInRequest &) -> std::optional<Avd::AuthorizationCallback> {
            return answer(query);
        });
        char *token = nullptr;
        QVERIFY(!request(broker, ACCESS_TOKEN_TYPE_AVD, &token, 0));
        QVERIFY(!broker.diagnostic().isEmpty());
        for (const QString &secret : secrets) {
            QVERIFY2(!broker.diagnostic().contains(secret, Qt::CaseInsensitive), qPrintable(broker.diagnostic()));
        }
    }
    // Well-known OAuth error codes stay in the diagnostic.
    Context ctx;
    RdpAccessTokenBroker broker(ctx.client());
    FakeOAuth oauth;
    oauth.settings = ctx.context->settings;
    oauth.install(broker);
    broker.setHelper(unavailableHelper());
    broker.setSignInPrompt([](const RdpAccessTokenBroker::SignInRequest &) -> std::optional<Avd::AuthorizationCallback> {
        return answer(QStringLiteral("error=access_denied&error_description=SYNTHETICSECRET&state=") + State);
    });
    char *token = nullptr;
    QVERIFY(!request(broker, ACCESS_TOKEN_TYPE_AVD, &token, 0));
    QVERIFY2(broker.diagnostic().contains(QStringLiteral("access_denied")), qPrintable(broker.diagnostic()));
    // The user still sees what the authority said.
    QVERIFY(broker.lastError().contains(QStringLiteral("SYNTHETICSECRET")));
}

void RdpAccessTokenTest::realFreeRdpAvdRequestsCarryPkce()
{
    // FreeRDP's own builder and extractor; only the token exchange, which needs the network, is replaced.
    Context ctx;
    RdpAccessTokenBroker broker(ctx.client());
    broker.setHelper(unavailableHelper());
    QByteArray tokenRequest;
    broker.setTokenExchange([&tokenRequest](const QByteArray &request, char **token) {
        tokenRequest = request;
        *token = strdup("synthetic-access-token");
        return true;
    });
    RdpAccessTokenBroker::SignInRequest seen;
    broker.setSignInPrompt([&seen](const RdpAccessTokenBroker::SignInRequest &request) -> std::optional<Avd::AuthorizationCallback> {
        seen = request;
        QString error;
        const auto callback = Avd::parseAuthorizationCallback((request.redirectUri + QStringLiteral("?code=synthetic-code&state=") + request.state).toUtf8(),
                                                              request.redirectUri,
                                                              request.state,
                                                              &error);
        if (!callback) {
            qWarning() << error;
        }
        return callback;
    });

    char *token = nullptr;
    QVERIFY2(request(broker, ACCESS_TOKEN_TYPE_AVD, &token, 0), qPrintable(broker.lastError()));
    free(token);

    const QUrlQuery authorize(seen.authorizeUrl);
    QCOMPARE(authorize.allQueryItemValues(QStringLiteral("client_id")), QStringList{ClientId});
    QCOMPARE(authorize.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded), Avd::brokerRedirectUri(ClientId));
    QCOMPARE(authorize.queryItemValue(QStringLiteral("state")), seen.state);
    QCOMPARE(authorize.queryItemValue(QStringLiteral("code_challenge_method")), QStringLiteral("S256"));
    const QByteArray challenge = authorize.queryItemValue(QStringLiteral("code_challenge")).toLatin1();
    QVERIFY(!challenge.isEmpty());

    // The token request proves possession of the verifier behind that challenge.
    const QUrlQuery tokenQuery(QString::fromLatin1(tokenRequest));
    QCOMPARE(tokenQuery.queryItemValue(QStringLiteral("code")), QStringLiteral("synthetic-code"));
    const QByteArray verifier = tokenQuery.queryItemValue(QStringLiteral("code_verifier"), QUrl::FullyDecoded).toLatin1();
    QVERIFY2(!verifier.isEmpty(), tokenRequest.constData());
    QCOMPARE(QCryptographicHash::hash(verifier, QCryptographicHash::Sha256).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals),
             challenge);
    QCOMPARE(tokenQuery.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded), Avd::brokerRedirectUri(ClientId));

    // FreeRDP's extractor itself refuses an answer with another state.
    QVERIFY(takeExtracted(ctx.client(), "state=00000000000000000000000000000000&code=other").isEmpty());
}

void RdpAccessTokenTest::cancelIsNotAnError()
{
    Context ctx;
    RdpAccessTokenBroker broker(ctx.client());
    FakeOAuth oauth;
    oauth.settings = ctx.context->settings;
    oauth.install(broker);
    broker.setHelper(unavailableHelper());
    broker.setSignInPrompt([](const RdpAccessTokenBroker::SignInRequest &) -> std::optional<Avd::AuthorizationCallback> {
        return std::nullopt;
    });

    char *token = nullptr;
    QVERIFY(!request(broker, ACCESS_TOKEN_TYPE_AVD, &token, 0));
    QVERIFY(broker.canceled());
    QVERIFY(oauth.exchanged.isEmpty());
}

void RdpAccessTokenTest::helperTokenSkipsBrowser()
{
    Context ctx;
    RdpAccessTokenBroker broker(ctx.client());
    FakeOAuth oauth;
    oauth.settings = ctx.context->settings;
    oauth.install(broker);
    QList<AccessTokenType> helperTypes;
    QList<QByteArray> helperScopes;
    broker.setHelper([&](AccessTokenType type, char **token, size_t count, va_list args) {
        helperTypes << type;
        if (type == ACCESS_TOKEN_TYPE_AAD && count == 2) {
            helperScopes << QByteArray(va_arg(args, const char *));
        }
        *token = strdup("helper-token");
        return RdpAccessTokenBroker::HelperResult::Succeeded;
    });
    bool prompted = false;
    broker.setSignInPrompt([&prompted](const RdpAccessTokenBroker::SignInRequest &) -> std::optional<Avd::AuthorizationCallback> {
        prompted = true;
        return std::nullopt;
    });

    char *token = nullptr;
    QVERIFY(request(broker, ACCESS_TOKEN_TYPE_AAD, &token, 2, HostScope.constData(), ReqCnf.constData()));
    QCOMPARE(QByteArray(token), QByteArray("helper-token"));
    free(token);
    QVERIFY(!prompted);
    QCOMPARE(helperTypes, QList<AccessTokenType>{ACCESS_TOKEN_TYPE_AAD});
    QCOMPARE(helperScopes, QList<QByteArray>{HostScope});
}

void RdpAccessTokenTest::failedHelperIsNotRetriedInBrowser()
{
    Context ctx;
    RdpAccessTokenBroker broker(ctx.client());
    FakeOAuth oauth;
    oauth.settings = ctx.context->settings;
    oauth.install(broker);
    broker.setHelper([](AccessTokenType, char **, size_t, va_list) {
        return RdpAccessTokenBroker::HelperResult::Failed;
    });
    bool prompted = false;
    broker.setSignInPrompt([&prompted](const RdpAccessTokenBroker::SignInRequest &) -> std::optional<Avd::AuthorizationCallback> {
        prompted = true;
        return std::nullopt;
    });

    char *token = nullptr;
    QVERIFY(!request(broker, ACCESS_TOKEN_TYPE_AVD, &token, 0));
    // The user closed the helper's window or it timed out; asking again would override that.
    QVERIFY(!prompted);
    QVERIFY(!broker.lastError().isEmpty());
}

void RdpAccessTokenTest::rejectsMissingArguments()
{
    Context ctx;
    RdpAccessTokenBroker broker(ctx.client());
    FakeOAuth oauth;
    oauth.settings = ctx.context->settings;
    oauth.install(broker);
    broker.setHelper(unavailableHelper());
    bool prompted = false;
    broker.setSignInPrompt([&prompted](const RdpAccessTokenBroker::SignInRequest &) -> std::optional<Avd::AuthorizationCallback> {
        prompted = true;
        return std::nullopt;
    });

    char *token = nullptr;
    QVERIFY(!request(broker, ACCESS_TOKEN_TYPE_AAD, &token, 0));
    QVERIFY(!prompted);
}

void RdpAccessTokenTest::freerdpHelperReportsUnavailable()
{
    Context ctx;
    // Points FreeRDP at a helper that does not exist, without searching the system.
    QVERIFY(freerdp_settings_set_string(ctx.context->settings, FreeRDP_AadAuthHelper, "/nonexistent/krdc-test-aad-helper"));
    auto helper = RdpAccessTokenBroker::freerdpHelper(ctx.client());
    char *token = nullptr;
    va_list empty{};
    QCOMPARE(helper(ACCESS_TOKEN_TYPE_AVD, &token, 0, empty), RdpAccessTokenBroker::HelperResult::Unavailable);
    QVERIFY(!token);
}

void RdpAccessTokenTest::autodetectedHelperIsNotStarted()
{
    // FreeRDP's xdg-open helper would register itself for ms-appx-web links without asking.
    for (const char *configured : {static_cast<const char *>(nullptr), "autodetect", "relative-helper"}) {
        Context ctx;
        QVERIFY(freerdp_settings_set_string(ctx.context->settings, FreeRDP_AadAuthHelper, configured));
        auto helper = RdpAccessTokenBroker::freerdpHelper(ctx.client());
        char *token = nullptr;
        va_list empty{};
        QCOMPARE(helper(ACCESS_TOKEN_TYPE_AVD, &token, 0, empty), RdpAccessTokenBroker::HelperResult::Unavailable);
        QVERIFY(!token);
        QVERIFY(!ctx.client()->aadHelper);
    }
}

void RdpAccessTokenTest::helperThatExitsDoesNotKillClient()
{
    if (!QFileInfo(QStringLiteral("/bin/false")).isExecutable()) {
        QSKIP("needs /bin/false");
    }
    Context ctx;
    // FreeRDP writes its handshake into the pipe of a helper that already exited.
    QVERIFY(freerdp_settings_set_string(ctx.context->settings, FreeRDP_AadAuthHelper, "/bin/false"));
    auto helper = RdpAccessTokenBroker::freerdpHelper(ctx.client());
    char *token = nullptr;
    va_list empty{};
    QCOMPARE(helper(ACCESS_TOKEN_TYPE_AVD, &token, 0, empty), RdpAccessTokenBroker::HelperResult::Unavailable);
    QVERIFY(!token);
}

void RdpAccessTokenTest::avdTokenOnlyForTrustedGateway_data()
{
    QTest::addColumn<QByteArray>("host");
    QTest::addColumn<uint>("port");

    QTest::newRow("attacker") << QByteArray("gateway.attacker.example") << 443u;
    QTest::newRow("microsoft-wide") << QByteArray("gateway.microsoft.com") << 443u;
    QTest::newRow("look-alike") << QByteArray("rdgateway.wvd.microsoft.com.attacker.example") << 443u;
    QTest::newRow("other-port") << QByteArray("rdgateway.wvd.microsoft.com") << 8443u;
    QTest::newRow("none") << QByteArray() << 443u;
}

void RdpAccessTokenTest::avdTokenOnlyForTrustedGateway()
{
    QFETCH(QByteArray, host);
    QFETCH(uint, port);

    Context ctx;
    ctx.setGateway(host.isEmpty() ? nullptr : host.constData(), port);
    RdpAccessTokenBroker broker(ctx.client());
    FakeOAuth oauth;
    oauth.settings = ctx.context->settings;
    oauth.install(broker);
    bool helperAsked = false;
    broker.setHelper([&helperAsked](AccessTokenType, char **token, size_t, va_list) {
        helperAsked = true;
        *token = strdup("helper-token");
        return RdpAccessTokenBroker::HelperResult::Succeeded;
    });
    bool prompted = false;
    broker.setSignInPrompt([&prompted](const RdpAccessTokenBroker::SignInRequest &) -> std::optional<Avd::AuthorizationCallback> {
        prompted = true;
        return answer(QStringLiteral("code=abc&state=") + State);
    });

    char *token = nullptr;
    QVERIFY(!request(broker, ACCESS_TOKEN_TYPE_AVD, &token, 0));
    QVERIFY(!token);
    QVERIFY(!helperAsked);
    QVERIFY(!prompted);
    QVERIFY(oauth.exchanged.isEmpty());
    QVERIFY(!broker.canceled());
    QVERIFY(!broker.lastError().isEmpty());
}

QTEST_GUILESS_MAIN(RdpAccessTokenTest)

#include "rdpaccesstokentest.moc"
