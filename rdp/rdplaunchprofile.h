/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include "avd/avdendpoints.h"
#include "connectionlaunch.h"

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <optional>

#include <freerdp/settings.h>

/**
 * The settings of an .rdp connection file, parsed with FreeRDP's own parser.
 *
 * The file contents are kept so the complete set of settings can be applied
 * to a session; a URL only identifies the connection. Nothing here is written
 * to disk by KRDC.
 */
class RdpLaunchProfile : public LaunchPayload
{
public:
    enum class Source {
        LocalFile,
        AvdWorkspace,
    };

    /** Largest connection file KRDC reads; real files are a few kilobytes. */
    static constexpr qsizetype MaxFileSize = 256 * 1024;

    static QString payloadKind();

    /** Parses UTF-8 (with or without BOM) or UTF-16LE (with BOM) contents. */
    static std::optional<RdpLaunchProfile> fromContents(const QByteArray &contents, Source source, QString *errorMessage);

    /** Reads an .rdp file. Other formats, including .rdpw, are refused with a reason. */
    static std::optional<RdpLaunchProfile> fromFile(const QString &path, QString *errorMessage);

    QString kind() const override;

    Source source() const;

    /** The computer or host pool to connect to, without the port. */
    QString address() const;
    /** The port, or -1 if the file does not set one. */
    int port() const;
    /** User name suggested by the file. It is only a default the user can change. */
    QString username() const;
    QString domain() const;
    QString gatewayHost() const;
    /** The gateway address and "gatewayusagemethod" as FreeRDP read them; the method is -1 if unset. */
    QString gatewayAddress() const;
    int gatewayUsageMethod() const;
    /** Whether the file asks to sign in to the remote computer with Microsoft Entra ID. */
    bool usesEntraAuthentication() const;
    bool isRemoteApp() const;
    bool isAzureVirtualDesktop() const;

    /** Identifies the connection, without user name or password. */
    QUrl displayUrl() const;

    /** UTF-8 contents as given to FreeRDP. */
    QByteArray contents() const;

    /**
     * Bearer token for the AVD gateway, kept in memory only. Refused, and
     * false returned, unless the file's gateway is an AVD gateway of @p cloud.
     */
    bool setGatewayBearerToken(const QString &token, const Avd::CloudEndpoints &cloud);
    bool hasGatewayBearerToken() const;

    /**
     * Applies all settings of the file to @p settings, then restores the
     * certificate checks the file may have relaxed.
     */
    bool applyTo(rdpSettings *settings, QString *errorMessage) const;

private:
    RdpLaunchProfile() = default;

    Source m_source = Source::LocalFile;
    QByteArray m_contents;
    QString m_address;
    int m_port = -1;
    QString m_username;
    QString m_domain;
    QString m_gatewayHost;
    int m_gatewayPort = -1;
    QString m_gatewayAddress;
    int m_gatewayUsageMethod = -1;
    bool m_entraAuthentication = false;
    bool m_remoteApp = false;
    bool m_avd = false;
    QString m_gatewayBearerToken;
    std::optional<Avd::CloudEndpoints> m_gatewayBearerCloud;
};

/** Splits "host", "host:port", "[ipv6]" or "[ipv6]:port"; a bare IPv6 address has no port. */
bool rdpSplitAddress(const QString &address, QString *host, int *port);

/** A launch that is not remembered, as its URL alone cannot reproduce it. */
ConnectionLaunch rdpLaunchFromProfile(const std::shared_ptr<const RdpLaunchProfile> &profile, const QString &name);

/**
 * Loads a local .rdp file for launching. Returns an invalid launch without a
 * message for files that are not connection files. Remote applications are
 * refused, as KRDC can only show full desktops so far.
 */
ConnectionLaunch rdpLaunchFromFile(const QUrl &fileUrl, QString *errorMessage);
