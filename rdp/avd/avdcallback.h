/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include <QByteArray>
#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QString>

#include <chrono>
#include <optional>

class QLocalServer;
class QLocalSocket;
class QTimer;

namespace Avd
{

/**
 * The redirect URI FreeRDP uses for @p clientId unless it reads the answer
 * from a terminal, as registered for Microsoft's public Remote Desktop client:
 * ms-appx-web://Microsoft.AAD.BrokerPlugin/<client ID>.
 */
QString brokerRedirectUri(const QString &clientId);

/** The answer of the authority, as the browser handed it back. */
struct AuthorizationCallback {
    /** Authorization code, decoded; empty if the authority refused. */
    QString code;
    /** The code as it appeared in the link, for requests FreeRDP builds without encoding. */
    QByteArray encodedCode;
    QString state;
    /** OAuth error, such as access_denied; empty on success. */
    QString error;
    QString errorDescription;

    bool isError() const
    {
        return !error.isEmpty();
    }
};

/**
 * Accepts @p link only if it is exactly @p redirectUri with a query that has
 * each parameter at most once, @p expectedState, and either a code or an
 * error. Anything else is refused with a reason in @p errorMessage, which
 * never contains the code or state.
 */
std::optional<AuthorizationCallback>
parseAuthorizationCallback(const QByteArray &link, const QString &redirectUri, const QString &expectedState, QString *errorMessage);

/**
 * Waits for krdc-avd-signin-handler to deliver the answer to one sign-in.
 *
 * Listens on a socket only the user can reach, for the sign-in's state, only
 * while it exists. Any program of the user can find the socket, so each
 * client is handled on its own: a malformed, oversized or stalled one is
 * refused and dropped, and the sign-in keeps waiting. At most MaxClients
 * are read at once, each for at most the client timeout; more are turned
 * away at once. The first valid link ends the wait. Canceling, finishing,
 * the overall timeout or destroying it drops all clients and removes the
 * socket, so later and repeated links reach nothing.
 */
class CallbackReceiver : public QObject
{
    Q_OBJECT

public:
    static constexpr std::chrono::minutes DefaultTimeout{10};
    static constexpr int MaxClients = 4;

    CallbackReceiver(const QString &redirectUri, const QString &state, QObject *parent = nullptr);
    ~CallbackReceiver() override;

    void setTimeout(std::chrono::milliseconds timeout);
    /** How long each client may take to send its link; for clients connecting afterwards. */
    void setClientTimeout(std::chrono::milliseconds timeout);

    /** Starts listening; reports why it cannot in @p errorMessage. */
    bool listen(QString *errorMessage);
    bool isListening() const;

    /** Stops listening without emitting anything. */
    void cancel();

Q_SIGNALS:
    void received(const Avd::AuthorizationCallback &callback);
    void failed(const QString &message);

private:
    void acceptConnection();
    void readLink(QLocalSocket *client);
    /** Forgets @p client and closes it, after sending @p reply if there is one. */
    void dropClient(QLocalSocket *client, const QByteArray &reply = {});
    void finish();
    void fail(const QString &message);

    QString m_redirectUri;
    QString m_state;
    QString m_path;
    std::chrono::milliseconds m_timeout = DefaultTimeout;
    std::chrono::milliseconds m_clientTimeout;
    QLocalServer *m_server = nullptr;
    /** What each connected client sent so far. */
    QHash<QLocalSocket *, QByteArray> m_clients;
    QTimer *m_deadline;
};

} // namespace Avd

Q_DECLARE_METATYPE(Avd::AuthorizationCallback)
