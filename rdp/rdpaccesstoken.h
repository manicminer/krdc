/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include "avd/avdcallback.h"

#include <QString>
#include <QUrl>

#include <cstdarg>
#include <functional>
#include <optional>

#include <freerdp/client.h>
#include <freerdp/freerdp.h>

/**
 * Answers FreeRDP's GetAccessToken requests for Microsoft Entra ID.
 *
 * FreeRDP builds the sign-in and token requests with their OAuth state and
 * PKCE, and exchanges the code, including the proof-of-possession key a
 * remote computer requires. This class only gets the authorization code to
 * FreeRDP: through a FreeRDP sign-in helper the user configured explicitly,
 * or else by letting the user sign in with their own browser, which hands
 * the answer back through FreeRDP's ms-appx-web: redirect.
 */
class RdpAccessTokenBroker
{
public:
    enum class Kind {
        /** Token for the Azure Virtual Desktop gateway. */
        AvdGateway,
        /** Token for signing in to the remote computer itself. */
        RemoteComputer,
    };

    struct SignInRequest {
        Kind kind = Kind::RemoteComputer;
        QUrl authorizeUrl;
        QString authorityHost;
        /** The redirect and state of FreeRDP's request the answer has to match. */
        QString redirectUri;
        QString state;
    };

    enum class HelperResult {
        Unavailable,
        Succeeded,
        Failed,
    };

    /** Returns the answer the browser handed back, or nothing if the user canceled. */
    using SignInPrompt = std::function<std::optional<Avd::AuthorizationCallback>(const SignInRequest &request)>;
    using Helper = std::function<HelperResult(AccessTokenType type, char **token, size_t count, va_list args)>;
    /** Builds a request with FreeRDP's OAuth state; @p code and @p reqCnf are only used for token requests. */
    using RequestBuilder = std::function<QByteArray(freerdp_client_aad_type type, const QByteArray &scope, const QByteArray &code, const QByteArray &reqCnf)>;
    /** Returns the code if the answer carries FreeRDP's state, otherwise an empty array. */
    using CodeExtractor = std::function<QByteArray(const QByteArray &redirect)>;
    using TokenExchange = std::function<bool(const QByteArray &tokenRequest, char **token)>;

    explicit RdpAccessTokenBroker(rdpClientContext *context);

    void setSignInPrompt(SignInPrompt prompt);
    void setHelper(Helper helper);
    void setRequestBuilder(RequestBuilder builder);
    void setCodeExtractor(CodeExtractor extractor);
    void setTokenExchange(TokenExchange exchange);

    /**
     * FreeRDP's helper process, which is reused for later requests of the
     * context. Only a helper configured by absolute path is started: FreeRDP's
     * autodetection could pick one that registers itself for ms-appx-web:
     * links without asking.
     */
    static Helper freerdpHelper(rdpClientContext *context);

    bool getAccessToken(AccessTokenType type, char **token, size_t count, va_list args);

    bool canceled() const;
    /**
     * Why the last request failed, for the user. Never contains tokens or
     * codes, but may contain the authority's description of a refusal, which
     * the browser handed back and which may quote anything.
     */
    QString lastError() const;
    /** Why the last request failed, for logs: fixed texts only, nothing from the answer. */
    QString diagnostic() const;

private:
    bool signInWithBrowser(Kind kind, const char *scope, const char *reqCnf, char **token);

    rdpClientContext *m_context;
    SignInPrompt m_prompt;
    Helper m_helper;
    RequestBuilder m_builder;
    CodeExtractor m_extractor;
    TokenExchange m_exchange;
    bool m_canceled = false;
    QString m_lastError;
    QString m_diagnostic;
};
