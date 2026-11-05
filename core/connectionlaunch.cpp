/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

#include "connectionlaunch.h"

LaunchPayload::~LaunchPayload() = default;

ConnectionLaunch ConnectionLaunch::fromUrl(const QUrl &url)
{
    ConnectionLaunch launch;
    launch.url = url;
    return launch;
}

bool ConnectionLaunch::isValid() const
{
    return url.isValid() && !url.isEmpty();
}
