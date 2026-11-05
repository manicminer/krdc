/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "avd/avdtokenstore.h"

#include "krdc_rdpcore_debug.h"

#include <qt6keychain/keychain.h>

namespace Avd
{

namespace
{
const QString Service = QStringLiteral("KRDC Azure Virtual Desktop");
}

TokenStore::~TokenStore() = default;

KeychainTokenStore::KeychainTokenStore(const QString &authorityHost)
    : m_authorityHost(authorityHost)
{
}

QString KeychainTokenStore::keyFor(const QString &account) const
{
    return QStringLiteral("refresh-token/%1/%2").arg(m_authorityHost, account.toLower());
}

void KeychainTokenStore::read(const QString &account, ReadCallback callback)
{
    auto *job = new QKeychain::ReadPasswordJob(Service);
    job->setKey(keyFor(account));
    // Never fall back to an unencrypted file.
    job->setInsecureFallback(false);
    QObject::connect(job, &QKeychain::Job::finished, job, [job, callback]() {
        if (job->error() != QKeychain::NoError && job->error() != QKeychain::EntryNotFound) {
            qCWarning(KRDC_RDP) << "Could not read the workspace sign-in from the keychain:" << job->errorString();
        }
        callback(job->error() == QKeychain::NoError ? job->textData() : QString());
    });
    job->start();
}

void KeychainTokenStore::write(const QString &account, const QString &refreshToken)
{
    auto *job = new QKeychain::WritePasswordJob(Service);
    job->setKey(keyFor(account));
    job->setInsecureFallback(false);
    job->setTextData(refreshToken);
    QObject::connect(job, &QKeychain::Job::finished, job, [job]() {
        if (job->error() != QKeychain::NoError) {
            qCWarning(KRDC_RDP) << "Could not keep the workspace sign-in in the keychain:" << job->errorString();
        }
    });
    job->start();
}

void KeychainTokenStore::remove(const QString &account)
{
    auto *job = new QKeychain::DeletePasswordJob(Service);
    job->setKey(keyFor(account));
    job->setInsecureFallback(false);
    job->start();
}

} // namespace Avd
