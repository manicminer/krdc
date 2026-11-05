/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include "avdcallback.h"
#include "avdhandlerregistration.h"

#include <QDialog>
#include <QUrl>

#include <chrono>
#include <functional>
#include <optional>

class QLabel;
class QPushButton;

/**
 * Lets the user sign in to Microsoft Entra ID with their own browser.
 *
 * The sign-in ends with the browser opening an ms-appx-web: link, the
 * redirect registered for the public client. krdc-avd-signin-handler passes
 * it to this dialog, which finishes as soon as the answer for its own sign-in
 * arrives. If ms-appx-web: links do not reach KRDC yet, the dialog offers to
 * set that up, and only does so once the user agreed.
 */
class AvdSignInDialog : public QDialog
{
    Q_OBJECT

public:
    struct Request {
        QUrl authorizeUrl;
        QString redirectUri;
        QString state;
        QString purpose;
    };

    /** Starts opening @p url and calls @p done once, with an empty string on success or the reason it failed. */
    using UrlOpener = std::function<void(const QUrl &url, QWidget *window, const std::function<void(const QString &error)> &done)>;
    /** Asks whether KRDC may become the handler of ms-appx-web: links, given who handles them now. */
    using SetupConfirmation = std::function<bool(const Avd::HandlerRegistration::Status &status, const QString &handlerPath)>;

    explicit AvdSignInDialog(const Request &request, QWidget *parent = nullptr);
    ~AvdSignInDialog() override;

    /** The default browser of the desktop, opened through KIO. */
    static UrlOpener systemBrowser();

    /** Replaceable for tests. */
    void setUrlOpener(UrlOpener opener);
    void setHandlerRegistration(const Avd::HandlerRegistration &registration);
    void setSetupConfirmation(SetupConfirmation confirmation);
    void setTimeout(std::chrono::milliseconds timeout);

    /** The answer of the authority, once the dialog was accepted. */
    std::optional<Avd::AuthorizationCallback> callback() const;

private:
    void openBrowser();
    void setUpHandler();
    void updateHandlerState();
    void showError(const QString &message);
    bool confirmSetup(const Avd::HandlerRegistration::Status &status, const QString &handlerPath);

    Request m_request;
    Avd::CallbackReceiver *m_receiver;
    Avd::HandlerRegistration m_registration;
    UrlOpener m_opener;
    SetupConfirmation m_confirmation;
    std::optional<Avd::AuthorizationCallback> m_callback;
    bool m_listening = false;
    bool m_failed = false;
    bool m_opening = false;

    QLabel *m_handlerLabel;
    QPushButton *m_setupButton;
    QPushButton *m_openButton;
    QLabel *m_statusLabel;
};
