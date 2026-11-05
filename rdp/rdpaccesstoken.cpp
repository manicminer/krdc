/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "rdpaccesstoken.h"

#include "avd/avdcallbackchannel.h"
#include "avd/avdendpoints.h"
#include "krdc_rdpcore_debug.h"

#include <KLocalizedString>

#include <QDir>
#include <QFileInfo>
#include <QUrlQuery>

#include <csignal>
#include <cstring>
#include <pthread.h>

#include <freerdp/client/aad_helper.h>
#include <winpr/crt.h>

namespace
{
QByteArray takeString(char *str)
{
    if (!str) {
        return {};
    }
    QByteArray result(str);
    winpr_zfree(str);
    return result;
}

void wipe(QByteArray &data)
{
    if (!data.isEmpty()) {
        SecureZeroMemory(data.data(), static_cast<size_t>(data.size()));
        data.clear();
    }
}

// FreeRDP writes to the helper's pipe even after the helper died, which
// would kill the whole application with SIGPIPE. The write fails instead.
class SigpipeBlocker
{
public:
    SigpipeBlocker()
    {
        sigset_t pipe;
        sigemptyset(&pipe);
        sigaddset(&pipe, SIGPIPE);
        m_active = pthread_sigmask(SIG_BLOCK, &pipe, &m_saved) == 0;
    }
    ~SigpipeBlocker()
    {
        if (!m_active) {
            return;
        }
        sigset_t pipe;
        sigemptyset(&pipe);
        sigaddset(&pipe, SIGPIPE);
        const timespec zero{0, 0};
        while (sigtimedwait(&pipe, nullptr, &zero) == SIGPIPE) { }
        pthread_sigmask(SIG_SETMASK, &m_saved, nullptr);
    }

private:
    sigset_t m_saved;
    bool m_active = false;
};

// The error codes of OAuth 2.0 and OpenID Connect; anything else the answer
// says is not repeated, as it may carry links, codes or tokens.
QString knownOAuthError(const QString &error)
{
    static const QStringList known{
        QStringLiteral("invalid_request"),
        QStringLiteral("unauthorized_client"),
        QStringLiteral("access_denied"),
        QStringLiteral("unsupported_response_type"),
        QStringLiteral("invalid_scope"),
        QStringLiteral("server_error"),
        QStringLiteral("temporarily_unavailable"),
        QStringLiteral("interaction_required"),
        QStringLiteral("login_required"),
        QStringLiteral("account_selection_required"),
        QStringLiteral("consent_required"),
        QStringLiteral("invalid_request_uri"),
        QStringLiteral("invalid_request_object"),
        QStringLiteral("request_not_supported"),
        QStringLiteral("request_uri_not_supported"),
        QStringLiteral("registration_not_supported"),
    };
    return known.contains(error) ? error : QStringLiteral("(not a standard error code)");
}

// FreeRDP uses its ms-appx-web: redirect, the one handed back to KRDC, only
// without its terminal callbacks; otherwise it asks for the nativeclient page.
class BrokerRedirectScope
{
public:
    explicit BrokerRedirectScope(rdpSettings *settings)
        : m_settings(settings)
        , m_saved(freerdp_settings_get_bool(settings, FreeRDP_UseCommonStdioCallbacks))
    {
        (void)freerdp_settings_set_bool(m_settings, FreeRDP_UseCommonStdioCallbacks, FALSE);
    }
    ~BrokerRedirectScope()
    {
        (void)freerdp_settings_set_bool(m_settings, FreeRDP_UseCommonStdioCallbacks, m_saved);
    }

private:
    rdpSettings *m_settings;
    BOOL m_saved;
};
}

RdpAccessTokenBroker::RdpAccessTokenBroker(rdpClientContext *context)
    : m_context(context)
{
    m_helper = freerdpHelper(context);
    m_builder = [context](freerdp_client_aad_type type, const QByteArray &scope, const QByteArray &code, const QByteArray &reqCnf) {
        switch (type) {
        case FREERDP_CLIENT_AAD_AUTH_REQUEST:
            return takeString(freerdp_client_get_aad_url(context, type, scope.constData()));
        case FREERDP_CLIENT_AAD_TOKEN_REQUEST:
            return takeString(freerdp_client_get_aad_url(context, type, scope.constData(), code.constData(), reqCnf.constData()));
        case FREERDP_CLIENT_AAD_AVD_AUTH_REQUEST:
            return takeString(freerdp_client_get_aad_url(context, type));
        case FREERDP_CLIENT_AAD_AVD_TOKEN_REQUEST:
            return takeString(freerdp_client_get_aad_url(context, type, code.constData()));
        }
        return QByteArray();
    };
    m_extractor = [context](const QByteArray &redirect) {
        return takeString(freerdp_client_extract_aad_code(context, redirect.constData(), static_cast<size_t>(redirect.size())));
    };
    m_exchange = [context](const QByteArray &tokenRequest, char **token) {
        return client_common_get_access_token(context->context.instance, tokenRequest.constData(), token) == TRUE;
    };
}

void RdpAccessTokenBroker::setSignInPrompt(SignInPrompt prompt)
{
    m_prompt = std::move(prompt);
}

void RdpAccessTokenBroker::setHelper(Helper helper)
{
    m_helper = std::move(helper);
}

void RdpAccessTokenBroker::setRequestBuilder(RequestBuilder builder)
{
    m_builder = std::move(builder);
}

void RdpAccessTokenBroker::setCodeExtractor(CodeExtractor extractor)
{
    m_extractor = std::move(extractor);
}

void RdpAccessTokenBroker::setTokenExchange(TokenExchange exchange)
{
    m_exchange = std::move(exchange);
}

RdpAccessTokenBroker::Helper RdpAccessTokenBroker::freerdpHelper(rdpClientContext *context)
{
    return [context](AccessTokenType type, char **token, size_t count, va_list args) {
        const SigpipeBlocker sigpipeBlocker;
        if (!context->aadHelper) {
            const char *configured = freerdp_settings_get_string(context->context.settings, FreeRDP_AadAuthHelper);
            if (!configured || !QDir::isAbsolutePath(QString::fromUtf8(configured))) {
                return HelperResult::Unavailable;
            }
            if (!QFileInfo(QString::fromUtf8(configured)).isExecutable()) {
                qCWarning(KRDC_RDP) << "The configured FreeRDP sign-in helper cannot be run:" << configured;
                return HelperResult::Unavailable;
            }
            context->aadHelper = aad_auth_helper_start(context);
            if (!context->aadHelper) {
                return HelperResult::Unavailable;
            }
        }
        va_list copy;
        va_copy(copy, args);
        const BOOL ok = aad_auth_helper_get_access_token_v(context->aadHelper, type, token, count, copy);
        va_end(copy);
        if (ok && *token) {
            return HelperResult::Succeeded;
        }
        // Started again for the next request, in case it exited.
        aad_auth_helper_stop(context->aadHelper);
        context->aadHelper = nullptr;
        return HelperResult::Failed;
    };
}

bool RdpAccessTokenBroker::getAccessToken(AccessTokenType type, char **token, size_t count, va_list args)
{
    *token = nullptr;
    m_canceled = false;
    m_lastError.clear();
    m_diagnostic.clear();

    Kind kind;
    const char *scope = "";
    const char *reqCnf = "";
    switch (type) {
    case ACCESS_TOKEN_TYPE_AVD: {
        kind = Kind::AvdGateway;
        // FreeRDP sends this token to the gateway the connection file names.
        const rdpSettings *settings = m_context->context.settings;
        const QString gateway = QString::fromUtf8(freerdp_settings_get_string(settings, FreeRDP_GatewayHostname));
        if (!Avd::isTrustedAvdGateway(gateway, static_cast<int>(freerdp_settings_get_uint32(settings, FreeRDP_GatewayPort)), Avd::publicCloud())) {
            m_lastError = i18nc("@info", "The gateway of the connection is not an Azure Virtual Desktop gateway; KRDC does not sign in to it.");
            return false;
        }
        break;
    }
    case ACCESS_TOKEN_TYPE_AAD: {
        kind = Kind::RemoteComputer;
        if (count < 2) {
            m_lastError = i18nc("@info", "The remote computer asked for a Microsoft Entra sign-in without saying what for.");
            return false;
        }
        va_list copy;
        va_copy(copy, args);
        scope = va_arg(copy, const char *);
        reqCnf = va_arg(copy, const char *);
        va_end(copy);
        if (!scope || !reqCnf) {
            m_lastError = i18nc("@info", "The remote computer asked for a Microsoft Entra sign-in without saying what for.");
            return false;
        }
        break;
    }
    default:
        m_lastError = i18nc("@info", "The connection asked for an unknown kind of sign-in.");
        return false;
    }

    if (m_helper) {
        va_list copy;
        va_copy(copy, args);
        const HelperResult result = m_helper(type, token, count, copy);
        va_end(copy);
        switch (result) {
        case HelperResult::Succeeded:
            return true;
        case HelperResult::Failed:
            // Canceled, timed out or refused there: the user already decided.
            m_lastError = i18nc("@info", "Signing in with the FreeRDP sign-in helper did not succeed.");
            return false;
        case HelperResult::Unavailable:
            qCDebug(KRDC_RDP) << "No FreeRDP sign-in helper, signing in with the browser";
            break;
        }
    }

    return signInWithBrowser(kind, scope, reqCnf, token);
}

bool RdpAccessTokenBroker::signInWithBrowser(Kind kind, const char *scope, const char *reqCnf, char **token)
{
    if (!m_prompt) {
        m_lastError = i18nc("@info", "There is no way to sign in to Microsoft Entra ID here.");
        return false;
    }

    rdpSettings *settings = m_context->context.settings;
    const BrokerRedirectScope redirectScope(settings);

    const bool gateway = kind == Kind::AvdGateway;
    const QByteArray scopeArg(scope);
    const QByteArray reqCnfArg(reqCnf);

    QByteArray authorize = m_builder(gateway ? FREERDP_CLIENT_AAD_AVD_AUTH_REQUEST : FREERDP_CLIENT_AAD_AUTH_REQUEST, scopeArg, {}, {});
    QString authority = QString::fromUtf8(freerdp_settings_get_string(settings, FreeRDP_GatewayAzureActiveDirectory));
    if (authority.isEmpty()) {
        authority = Avd::publicCloud().authorityHost;
    }
    const QString clientId = QString::fromUtf8(freerdp_settings_get_string(settings, FreeRDP_GatewayAvdClientID));
    const QString redirectUri = Avd::brokerRedirectUri(clientId);
    const QUrl authorizeUrl = QUrl::fromEncoded(authorize, QUrl::StrictMode);
    wipe(authorize);

    // Only an authorization code request of the client whose redirect is
    // handed back to KRDC, with state and S256 PKCE, that ends on that redirect.
    const QUrlQuery query(authorizeUrl);
    const auto single = [&query](const QString &key) {
        const QStringList values = query.allQueryItemValues(key, QUrl::FullyDecoded);
        return values.size() == 1 ? values.first() : QString();
    };
    const QString state = single(QStringLiteral("state"));
    if (!authorizeUrl.isValid() || authorizeUrl.scheme() != QLatin1String("https") || authorizeUrl.host().compare(authority, Qt::CaseInsensitive) != 0
        || !authorizeUrl.userInfo().isEmpty() || clientId.isEmpty() || single(QStringLiteral("client_id")) != clientId
        || single(QStringLiteral("response_type")) != QLatin1String("code") || single(QStringLiteral("redirect_uri")) != redirectUri
        || !Avd::CallbackChannel::isValidState(state) || single(QStringLiteral("code_challenge")).isEmpty()
        || single(QStringLiteral("code_challenge_method")) != QLatin1String("S256")) {
        m_lastError = i18nc("@info", "The Microsoft Entra sign-in could not be prepared.");
        return false;
    }

    const std::optional<Avd::AuthorizationCallback> callback = m_prompt(SignInRequest{kind, authorizeUrl, authority, redirectUri, state});
    if (!callback) {
        m_canceled = true;
        return false;
    }
    if (callback->state != state) {
        m_lastError = i18nc("@info", "The sign-in answer does not belong to this connection. Sign in again.");
        return false;
    }
    if (callback->isError()) {
        // For the user, who may need the authority's explanation; the log gets no text of the answer.
        m_lastError = i18nc("@info",
                            "Microsoft Entra ID did not allow the sign-in: %1",
                            callback->errorDescription.isEmpty() ? callback->error : callback->errorDescription);
        m_diagnostic = QStringLiteral("Microsoft Entra ID refused the sign-in with error %1").arg(knownOAuthError(callback->error));
        return false;
    }

    // FreeRDP finds the first "state=" and "code=" in what it gets, so it only gets these two.
    QByteArray response = "state=" + state.toUtf8() + "&code=" + callback->encodedCode;
    QByteArray code = m_extractor(response);
    wipe(response);
    if (code.isEmpty()) {
        m_lastError = i18nc("@info", "The sign-in answer does not belong to this connection. Sign in again.");
        return false;
    }

    QByteArray tokenRequest = m_builder(gateway ? FREERDP_CLIENT_AAD_AVD_TOKEN_REQUEST : FREERDP_CLIENT_AAD_TOKEN_REQUEST, scopeArg, code, reqCnfArg);
    wipe(code);
    const bool ok = !tokenRequest.isEmpty() && m_exchange(tokenRequest, token) && *token;
    wipe(tokenRequest);
    if (!ok) {
        free(*token);
        *token = nullptr;
        m_lastError = i18nc("@info", "Microsoft Entra ID did not issue a token for this connection.");
        return false;
    }
    return true;
}

bool RdpAccessTokenBroker::canceled() const
{
    return m_canceled;
}

QString RdpAccessTokenBroker::lastError() const
{
    return m_lastError;
}

QString RdpAccessTokenBroker::diagnostic() const
{
    // All other messages are fixed texts of KRDC.
    return m_diagnostic.isEmpty() ? m_lastError : m_diagnostic;
}
