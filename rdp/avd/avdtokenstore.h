/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace Avd
{

/** Stores workspace refresh tokens. Implementations must keep them out of plain-text configuration. */
class TokenStore
{
public:
    virtual ~TokenStore();

    using ReadCallback = std::function<void(const QString &refreshToken)>;

    /** Calls @p callback with the stored token, or an empty string. */
    virtual void read(const QString &account, ReadCallback callback) = 0;
    virtual void write(const QString &account, const QString &refreshToken) = 0;
    virtual void remove(const QString &account) = 0;
};

/** Keeps refresh tokens in the system keychain through QtKeychain. */
class KeychainTokenStore : public TokenStore
{
public:
    explicit KeychainTokenStore(const QString &authorityHost);

    void read(const QString &account, ReadCallback callback) override;
    void write(const QString &account, const QString &refreshToken) override;
    void remove(const QString &account) override;

    QString keyFor(const QString &account) const;

private:
    QString m_authorityHost;
};

} // namespace Avd
