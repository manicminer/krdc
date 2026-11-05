/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "rdpuihandoff.h"

void RdpUiHandoff::cancel()
{
    std::lock_guard lock(m_state->mutex);
    m_state->canceled = true;
    m_state->changed.notify_all();
}

void RdpUiHandoff::reset()
{
    // A fresh state: tasks still queued from before stay canceled.
    m_state = std::make_shared<State>();
}
