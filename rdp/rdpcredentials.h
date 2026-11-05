/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include <QString>

#include <optional>

#include <freerdp/freerdp.h>

/**
 * Keeps the credentials for the remote computer apart from those for the
 * gateway, and decides which to offer when FreeRDP asks for credentials.
 */
class RdpCredentialRouter
{
public:
    enum class Target {
        Host,
        Gateway,
        SmartcardPin,
        Unsupported,
    };

    struct Credentials {
        QString username;
        QString domain;
        QString password;
    };

    struct Prompt {
        Target target = Target::Host;
        QString username;
        QString domain;
        /** The user name is a suggestion and the user may change it. */
        bool usernameEditable = true;
        bool showUsername = true;
        bool showDomain = true;
        /**
         * The remote computer was set up for Microsoft Entra sign-in but
         * asked for a password: its policy requires a different logon.
         */
        bool entraExpected = false;
    };

    static Target targetForReason(rdp_auth_reason reason);

    void setHostCredentials(const Credentials &credentials);
    Credentials hostCredentials() const;

    void setGatewayCredentials(const Credentials &credentials);
    Credentials gatewayCredentials() const;

    /** With the gateway set to use the same login, gateway prompts use the host credentials. */
    void setGatewayUsesHostCredentials(bool same);
    void setEntraExpected(bool expected);

    /**
     * Credentials to answer with without asking, at most once per target and
     * only when a password is already known.
     */
    std::optional<Credentials> automaticAnswer(Target target);

    Prompt promptFor(Target target) const;

    /** Stores what the user entered for @p target, leaving the other target alone. */
    void accept(Target target, const Credentials &credentials);

private:
    Credentials m_host;
    Credentials m_gateway;
    bool m_gatewaySame = false;
    bool m_entraExpected = false;
    bool m_hostTried = false;
    bool m_gatewayTried = false;
};
