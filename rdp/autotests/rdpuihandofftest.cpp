/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "rdpuihandoff.h"

#include <QTest>
#include <QThread>

#include <atomic>
#include <thread>

class RdpUiHandoffTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void answersOnContextThread();
    void cancelEndsQueuedRequest();
    void cancelEndsRequestWhileAnswering();
    void destroyedContextEndsRequest();
    void resetAcceptsRequestsAgain();
};

void RdpUiHandoffTest::answersOnContextThread()
{
    RdpUiHandoff handoff;
    QObject context;
    std::optional<int> result;
    std::atomic_bool returned = false;
    Qt::HANDLE answeredOn = nullptr;
    std::thread worker([&]() {
        result = handoff.request<int>(&context, [&answeredOn]() {
            answeredOn = QThread::currentThreadId();
            return 42;
        });
        returned = true;
    });
    QTRY_VERIFY(returned);
    worker.join();
    QVERIFY(result);
    QCOMPARE(*result, 42);
    QCOMPARE(answeredOn, QThread::currentThreadId());
}

void RdpUiHandoffTest::cancelEndsQueuedRequest()
{
    RdpUiHandoff handoff;
    QObject context;
    bool ran = false;
    std::atomic_bool returned = false;
    std::optional<int> result = 0;
    std::thread worker([&]() {
        result = handoff.request<int>(&context, [&ran]() {
            ran = true;
            return 1;
        });
        returned = true;
    });
    // This thread does not process the request, as when it joins the worker.
    QThread::msleep(100);
    QVERIFY(!returned);
    handoff.cancel();
    worker.join();
    QVERIFY(!result);

    // Delivered later, a canceled request does nothing.
    QCoreApplication::processEvents();
    QVERIFY(!ran);
}

void RdpUiHandoffTest::cancelEndsRequestWhileAnswering()
{
    RdpUiHandoff handoff;
    QObject context;
    std::optional<int> result = 0;
    std::atomic_bool answering = false;
    std::thread worker;
    worker = std::thread([&]() {
        result = handoff.request<int>(&context, [&]() {
            answering = true;
            // Stands in for a dialog's event loop, during which the view is closed.
            handoff.cancel();
            worker.join();
            return 1;
        });
    });
    QTRY_VERIFY(answering);
    QVERIFY(!result);
}

void RdpUiHandoffTest::destroyedContextEndsRequest()
{
    RdpUiHandoff handoff;
    auto *context = new QObject;
    std::optional<int> result = 0;
    std::atomic_bool returned = false;
    std::thread worker([&]() {
        result = handoff.request<int>(context, []() {
            return 1;
        });
        returned = true;
    });
    QThread::msleep(100);
    delete context;
    for (int i = 0; i < 100 && !returned; ++i) {
        QThread::msleep(20);
    }
    QVERIFY(returned);
    worker.join();
    QVERIFY(!result);
}

void RdpUiHandoffTest::resetAcceptsRequestsAgain()
{
    RdpUiHandoff handoff;
    QObject context;
    handoff.cancel();
    QVERIFY(!handoff.request<int>(&context, []() {
        return 1;
    }));
    handoff.reset();
    std::optional<int> result;
    std::atomic_bool returned = false;
    std::thread worker([&]() {
        result = handoff.request<int>(&context, []() {
            return 2;
        });
        returned = true;
    });
    QTRY_VERIFY(returned);
    worker.join();
    QVERIFY(result);
    QCOMPARE(*result, 2);
}

QTEST_GUILESS_MAIN(RdpUiHandoffTest)

#include "rdpuihandofftest.moc"
