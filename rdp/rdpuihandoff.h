/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#pragma once

#include <QMetaObject>
#include <QObject>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

/**
 * Lets FreeRDP's thread ask the GUI thread something, such as credentials,
 * and wait for the answer, like a BlockingQueuedConnection.
 *
 * Unlike one, the wait ends when cancel() is called, so the GUI thread can
 * stop and join the FreeRDP thread while a request is queued or a dialog for
 * it is still open. The wait also ends if the context object is destroyed
 * before the request is delivered.
 */
class RdpUiHandoff
{
public:
    /**
     * Runs @p task on the thread of @p context and returns its result, or
     * nothing if the handoff was canceled first. Called from another thread.
     */
    template<typename T>
    std::optional<T> request(QObject *context, std::function<T()> task);

    /** Ends all waiting and later requests; called from the GUI thread before joining. */
    void cancel();
    /** Accepts requests again; only while no other thread uses the handoff. */
    void reset();

private:
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        bool canceled = false;
    };

    template<typename T>
    struct Call {
        std::optional<T> result;
        bool finished = false;
    };

    // Marks the call finished when the last copy of the posted task goes away,
    // whether it ran or was dropped with its context.
    template<typename T>
    class Finisher
    {
    public:
        Finisher(std::shared_ptr<State> state, std::shared_ptr<Call<T>> call)
            : m_state(std::move(state))
            , m_call(std::move(call))
        {
        }
        Finisher(const Finisher &) = delete;
        Finisher &operator=(const Finisher &) = delete;
        ~Finisher()
        {
            std::lock_guard lock(m_state->mutex);
            m_call->finished = true;
            m_state->changed.notify_all();
        }

    private:
        std::shared_ptr<State> m_state;
        std::shared_ptr<Call<T>> m_call;
    };

    std::shared_ptr<State> m_state = std::make_shared<State>();
};

template<typename T>
std::optional<T> RdpUiHandoff::request(QObject *context, std::function<T()> task)
{
    const std::shared_ptr<State> state = m_state;
    auto call = std::make_shared<Call<T>>();
    {
        std::lock_guard lock(state->mutex);
        if (state->canceled) {
            return std::nullopt;
        }
    }

    auto finisher = std::make_shared<Finisher<T>>(state, call);
    const bool posted = QMetaObject::invokeMethod(
        context,
        [state, call, finisher, task = std::move(task)]() {
            {
                std::lock_guard lock(state->mutex);
                if (state->canceled) {
                    return;
                }
            }
            T value = task();
            std::lock_guard lock(state->mutex);
            if (!state->canceled) {
                call->result = std::move(value);
            }
        },
        Qt::QueuedConnection);
    finisher.reset();
    if (!posted) {
        return std::nullopt;
    }

    std::unique_lock lock(state->mutex);
    state->changed.wait(lock, [&state, &call]() {
        return call->finished || state->canceled;
    });
    if (state->canceled) {
        return std::nullopt;
    }
    return std::move(call->result);
}
