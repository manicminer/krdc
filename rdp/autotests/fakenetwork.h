/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include <QBuffer>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

// Offline stand-in for the network: answers from a table and records requests.
class FakeReply : public QNetworkReply
{
public:
    FakeReply(QNetworkAccessManager::Operation operation,
              const QNetworkRequest &request,
              int status,
              const QByteArray &body,
              const QList<std::pair<QByteArray, QByteArray>> &headers,
              QObject *parent)
        : QNetworkReply(parent)
        , m_body(body)
    {
        setOperation(operation);
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        for (const auto &[name, value] : headers) {
            setRawHeader(name, value);
        }
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this]() {
            if (isFinished()) {
                return;
            }
            Q_EMIT metaDataChanged();
            Q_EMIT readyRead();
            if (isFinished()) {
                return;
            }
            Q_EMIT downloadProgress(m_body.size(), m_body.size());
            setFinished(true);
            Q_EMIT finished();
        });
    }

    void abort() override
    {
        if (isFinished()) {
            return;
        }
        setError(OperationCanceledError, QStringLiteral("aborted"));
        setFinished(true);
        Q_EMIT errorOccurred(OperationCanceledError);
        Q_EMIT finished();
    }

    qint64 bytesAvailable() const override
    {
        return m_body.size() - m_offset + QIODevice::bytesAvailable();
    }

    bool isSequential() const override
    {
        return true;
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 count = std::min<qint64>(maxSize, m_body.size() - m_offset);
        if (count <= 0) {
            return isFinished() ? -1 : 0;
        }
        memcpy(data, m_body.constData() + m_offset, count);
        m_offset += count;
        return count;
    }

private:
    QByteArray m_body;
    qint64 m_offset = 0;
};

class FakeNetwork : public QNetworkAccessManager
{
public:
    struct Route {
        int status = 200;
        QByteArray body;
        QList<std::pair<QByteArray, QByteArray>> headers;
    };

    struct Recorded {
        QNetworkRequest request;
        QByteArray body;
    };

    void route(const QUrl &url, int status, const QByteArray &body, const QList<std::pair<QByteArray, QByteArray>> &headers = {})
    {
        m_routes.insert(url, Route{status, body, headers});
    }

    QList<Recorded> requests;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *outgoingData) override
    {
        Recorded recorded{request, outgoingData ? outgoingData->readAll() : QByteArray()};
        requests.append(recorded);
        const Route route = m_routes.value(request.url(), Route{404, QByteArray("not found"), {}});
        return new FakeReply(operation, request, route.status, route.body, route.headers, this);
    }

private:
    QHash<QUrl, Route> m_routes;
};
