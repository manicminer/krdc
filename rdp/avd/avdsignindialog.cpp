/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdsignindialog.h"

#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenUrlJob>
#include <KLocalizedString>
#include <KMessageBox>

#include <QDialogButtonBox>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

AvdSignInDialog::AvdSignInDialog(const Request &request, QWidget *parent)
    : QDialog(parent)
    , m_request(request)
    , m_receiver(new Avd::CallbackReceiver(request.redirectUri, request.state, this))
    , m_opener(systemBrowser())
{
    setWindowTitle(i18nc("@title:window", "Sign In with Microsoft Entra ID"));

    auto *layout = new QVBoxLayout(this);

    auto *purposeLabel = new QLabel(request.purpose, this);
    purposeLabel->setWordWrap(true);
    QFont bold = purposeLabel->font();
    bold.setBold(true);
    purposeLabel->setFont(bold);
    layout->addWidget(purposeLabel);

    auto *steps = new QLabel(i18nc("@info",
                                   "<ol><li>Open the sign-in page in your browser and sign in.</li>"
                                   "<li>At the end your browser asks whether to open the link with KRDC. Allow it.</li>"
                                   "<li>KRDC continues on its own; there is nothing to copy.</li></ol>"),
                             this);
    steps->setWordWrap(true);
    layout->addWidget(steps);

    m_handlerLabel = new QLabel(this);
    m_handlerLabel->setObjectName(QStringLiteral("handlerLabel"));
    m_handlerLabel->setWordWrap(true);
    m_handlerLabel->setTextFormat(Qt::PlainText);
    layout->addWidget(m_handlerLabel);

    m_setupButton = new QPushButton(QIcon::fromTheme(QStringLiteral("configure")), i18nc("@action:button", "Set Up KRDC for Sign-in Links…"), this);
    m_setupButton->setObjectName(QStringLiteral("setupHandlerButton"));
    connect(m_setupButton, &QPushButton::clicked, this, &AvdSignInDialog::setUpHandler);
    layout->addWidget(m_setupButton);

    m_openButton = new QPushButton(QIcon::fromTheme(QStringLiteral("internet-web-browser")), i18nc("@action:button", "Open Sign-in Page in Browser"), this);
    m_openButton->setObjectName(QStringLiteral("openBrowserButton"));
    connect(m_openButton, &QPushButton::clicked, this, &AvdSignInDialog::openBrowser);
    layout->addWidget(m_openButton);

    auto *urlLabel =
        new QLabel(i18nc("@info", "If no browser opens, open this address in one yourself:<br/><tt>%1</tt>", m_request.authorizeUrl.toString().toHtmlEscaped()),
                   this);
    urlLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    urlLabel->setWordWrap(true);
    layout->addWidget(urlLabel);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("statusLabel"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setTextFormat(Qt::PlainText);
    layout->addWidget(m_statusLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_receiver, &Avd::CallbackReceiver::received, this, [this](const Avd::AuthorizationCallback &callback) {
        m_callback = callback;
        accept();
    });
    connect(m_receiver, &Avd::CallbackReceiver::failed, this, [this](const QString &message) {
        m_failed = true;
        showError(i18nc("@info", "%1 Close this window and sign in again.", message));
        updateHandlerState();
    });
    // Ending the dialog in any way ends the wait, so later answers reach nothing.
    connect(this, &QDialog::finished, m_receiver, &Avd::CallbackReceiver::cancel);

    QString error;
    m_listening = m_receiver->listen(&error);
    if (!m_listening) {
        m_failed = true;
        showError(error);
    }
    updateHandlerState();
}

AvdSignInDialog::~AvdSignInDialog()
{
    m_receiver->cancel();
}

AvdSignInDialog::UrlOpener AvdSignInDialog::systemBrowser()
{
    return [](const QUrl &url, QWidget *window, const std::function<void(const QString &error)> &done) {
        // What xdg-open does on KDE: the default browser, with its errors reported here.
        auto *job = new KIO::OpenUrlJob(url);
        job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::Flags{}, window));
        QObject::connect(job, &KJob::result, job, [job, done]() {
            done(job->error() ? job->errorString() : QString());
        });
        job->start();
    };
}

void AvdSignInDialog::setUrlOpener(UrlOpener opener)
{
    m_opener = std::move(opener);
}

void AvdSignInDialog::setHandlerRegistration(const Avd::HandlerRegistration &registration)
{
    m_registration = registration;
    updateHandlerState();
}

void AvdSignInDialog::setSetupConfirmation(SetupConfirmation confirmation)
{
    m_confirmation = std::move(confirmation);
}

void AvdSignInDialog::setTimeout(std::chrono::milliseconds timeout)
{
    m_receiver->setTimeout(timeout);
}

std::optional<Avd::AuthorizationCallback> AvdSignInDialog::callback() const
{
    return result() == QDialog::Accepted ? m_callback : std::nullopt;
}

void AvdSignInDialog::updateHandlerState()
{
    using State = Avd::HandlerRegistration::State;
    const Avd::HandlerRegistration::Status status = m_registration.status();
    switch (status.state) {
    case State::Registered:
        m_handlerLabel->clear();
        break;
    case State::NotRegistered:
        m_handlerLabel->setText(i18nc("@info",
                                      "The browser hands the sign-in back with an ms-appx-web: link, which no application on this computer opens yet. "
                                      "Set up KRDC to open these links."));
        break;
    case State::OtherHandler:
        m_handlerLabel->setText(i18nc("@info",
                                      "The browser hands the sign-in back with an ms-appx-web: link, which “%1” (%2) opens now. "
                                      "To sign in here, KRDC has to open these links instead.",
                                      status.currentName,
                                      status.currentDesktopFile));
        break;
    case State::OtherKrdcHandler:
        m_handlerLabel->setText(i18nc("@info",
                                      "ms-appx-web: links open with the sign-in helper of another KRDC installation or build (%1). "
                                      "Set up this KRDC to open them.",
                                      status.currentCommand));
        break;
    case State::HandlerUnusable:
        m_handlerLabel->setText(status.problem);
        break;
    }
    m_handlerLabel->setVisible(status.state != State::Registered);
    const bool canSetUp = status.state != State::Registered && status.state != State::HandlerUnusable;
    m_setupButton->setVisible(canSetUp);
    m_setupButton->setEnabled(canSetUp && !m_failed);
    m_openButton->setEnabled(status.state == State::Registered && m_listening && !m_failed && !m_opening);
}

bool AvdSignInDialog::confirmSetup(const Avd::HandlerRegistration::Status &status, const QString &handlerPath)
{
    if (m_confirmation) {
        return m_confirmation(status, handlerPath);
    }
    QString current;
    switch (status.state) {
    case Avd::HandlerRegistration::State::OtherHandler:
        current = i18nc("@info",
                        "Currently “%1” opens them (%2, command “%3”). It will no longer receive them; "
                        "you can change this back in the file associations of the desktop.",
                        status.currentName,
                        status.currentDesktopFile,
                        status.currentCommand);
        break;
    case Avd::HandlerRegistration::State::OtherKrdcHandler:
        current = i18nc("@info", "Currently another KRDC sign-in helper opens them (command “%1”).", status.currentCommand);
        break;
    default:
        current = i18nc("@info", "No application opens them now.");
        break;
    }
    const QString text = i18nc("@info",
                               "<p>KRDC will open all ms-appx-web: links for your user account with its sign-in helper:<br/><tt>%1</tt></p>"
                               "<p>%2</p>"
                               "<p>The helper only passes a link to the KRDC sign-in that is waiting for it, and keeps nothing.</p>",
                               handlerPath.toHtmlEscaped(),
                               current.toHtmlEscaped());
    return KMessageBox::warningContinueCancel(this,
                                              text,
                                              i18nc("@title:window", "Open Sign-in Links with KRDC"),
                                              KGuiItem(i18nc("@action:button", "Open Them with KRDC"), QStringLiteral("dialog-ok")),
                                              KStandardGuiItem::cancel(),
                                              QString(),
                                              KMessageBox::Notify | KMessageBox::Dangerous)
        == KMessageBox::Continue;
}

void AvdSignInDialog::setUpHandler()
{
    // Asked every time; the answer is not remembered.
    const Avd::HandlerRegistration::Status status = m_registration.status();
    if (status.state == Avd::HandlerRegistration::State::Registered || status.state == Avd::HandlerRegistration::State::HandlerUnusable) {
        updateHandlerState();
        return;
    }
    if (!confirmSetup(status, m_registration.handlerPath())) {
        return;
    }
    QString error;
    if (!m_registration.registerHandler(&error)) {
        showError(error);
    } else {
        m_statusLabel->clear();
    }
    updateHandlerState();
}

void AvdSignInDialog::openBrowser()
{
    if (!m_opener || m_opening || m_failed) {
        return;
    }
    m_opening = true;
    m_statusLabel->setText(i18nc("@info:status", "Opening your browser…"));
    updateHandlerState();
    QPointer<AvdSignInDialog> self(this);
    m_opener(m_request.authorizeUrl, this, [self](const QString &error) {
        if (!self) {
            return;
        }
        self->m_opening = false;
        if (self->m_failed) {
            self->updateHandlerState();
            return;
        }
        if (error.isEmpty()) {
            self->m_statusLabel->setText(i18nc("@info:status", "Waiting for you to finish signing in in your browser…"));
        } else {
            self->showError(i18nc("@info", "The browser could not be opened: %1", error));
        }
        self->updateHandlerState();
    });
}

void AvdSignInDialog::showError(const QString &message)
{
    m_statusLabel->setText(message);
}
