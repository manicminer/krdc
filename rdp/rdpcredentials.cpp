/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "rdpcredentials.h"

RdpCredentialRouter::Target RdpCredentialRouter::targetForReason(rdp_auth_reason reason)
{
    switch (reason) {
    case AUTH_NLA:
    case AUTH_TLS:
    case AUTH_RDP:
    case AUTH_RDSTLS:
        return Target::Host;
    case GW_AUTH_HTTP:
    case GW_AUTH_RDG:
    case GW_AUTH_RPC:
        return Target::Gateway;
    case AUTH_SMARTCARD_PIN:
        return Target::SmartcardPin;
    default:
        return Target::Unsupported;
    }
}

void RdpCredentialRouter::setHostCredentials(const Credentials &credentials)
{
    m_host = credentials;
}

RdpCredentialRouter::Credentials RdpCredentialRouter::hostCredentials() const
{
    return m_host;
}

void RdpCredentialRouter::setGatewayCredentials(const Credentials &credentials)
{
    m_gateway = credentials;
}

RdpCredentialRouter::Credentials RdpCredentialRouter::gatewayCredentials() const
{
    return m_gateway;
}

void RdpCredentialRouter::setGatewayUsesHostCredentials(bool same)
{
    m_gatewaySame = same;
}

void RdpCredentialRouter::setEntraExpected(bool expected)
{
    m_entraExpected = expected;
}

std::optional<RdpCredentialRouter::Credentials> RdpCredentialRouter::automaticAnswer(Target target)
{
    switch (target) {
    case Target::Host:
        if (m_hostTried || m_host.password.isEmpty()) {
            return std::nullopt;
        }
        m_hostTried = true;
        return m_host;
    case Target::Gateway: {
        const Credentials &credentials = (m_gatewaySame && m_gateway.username.isEmpty()) ? m_host : m_gateway;
        if (m_gatewayTried || credentials.password.isEmpty()) {
            return std::nullopt;
        }
        m_gatewayTried = true;
        return credentials;
    }
    default:
        return std::nullopt;
    }
}

RdpCredentialRouter::Prompt RdpCredentialRouter::promptFor(Target target) const
{
    Prompt prompt;
    prompt.target = target;
    switch (target) {
    case Target::Host:
        prompt.username = m_host.username;
        prompt.domain = m_host.domain;
        prompt.entraExpected = m_entraExpected;
        break;
    case Target::Gateway: {
        const Credentials &credentials = (m_gatewaySame && m_gateway.username.isEmpty()) ? m_host : m_gateway;
        prompt.username = credentials.username;
        prompt.domain = credentials.domain;
        break;
    }
    case Target::SmartcardPin:
        prompt.showUsername = false;
        prompt.showDomain = false;
        break;
    case Target::Unsupported:
        break;
    }
    return prompt;
}

void RdpCredentialRouter::accept(Target target, const Credentials &credentials)
{
    switch (target) {
    case Target::Host:
        m_host = credentials;
        break;
    case Target::Gateway:
        m_gateway = credentials;
        break;
    case Target::SmartcardPin:
        // A PIN is never reused as a password.
    case Target::Unsupported:
        break;
    }
}
