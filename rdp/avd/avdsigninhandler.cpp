/*
    SPDX-FileCopyrightText: 2026 KRDC contributors

    SPDX-License-Identifier: MIT
*/

// krdc-avd-signin-handler: started by the desktop with the ms-appx-web: link
// a browser ends a Microsoft Entra sign-in on, passes it to the KRDC sign-in
// it belongs to and exits. It keeps nothing and never prints the link.

#include "avd/avdcallbackchannel.h"

#include <QCoreApplication>
#include <QLocalSocket>

#include <cstdio>

using namespace Avd;

namespace
{
int refuse(const char *message)
{
    std::fprintf(stderr, "krdc-avd-signin-handler: %s\n", message);
    return 1;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "Usage: krdc-avd-signin-handler <ms-appx-web link>\n");
        return 2;
    }
    const QByteArray link(argv[1]);
    if (link.size() >= CallbackChannel::MaxMessageSize) {
        return refuse("the link is too long");
    }
    // Spaces are allowed as KIO decodes %20; a line break would end the message early.
    for (const char c : link) {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7f) {
            return refuse("the link contains characters links cannot have");
        }
    }
    const QString state = CallbackChannel::stateOf(link);
    if (state.isEmpty()) {
        return refuse("the link does not end a KRDC sign-in");
    }

    const char *notWaiting = "no KRDC sign-in is waiting for this link; it was finished, canceled or timed out";
    CallbackChannel::DirectoryError error;
    const QString directory = CallbackChannel::directory(false, &error);
    if (directory.isEmpty()) {
        return refuse(error == CallbackChannel::DirectoryError::Insecure ? "the runtime directory is accessible to other users" : notWaiting);
    }
    const QString path = directory + QLatin1Char('/') + CallbackChannel::socketName(state);
    if (!CallbackChannel::isOwnedSocket(path)) {
        return refuse(notWaiting);
    }

    QLocalSocket socket;
    socket.connectToServer(path);
    if (!socket.waitForConnected(CallbackChannel::IoTimeoutMs) || !CallbackChannel::peerIsSameUser(socket.socketDescriptor())) {
        return refuse(notWaiting);
    }
    socket.write(link + '\n');
    while (socket.bytesToWrite() > 0) {
        if (!socket.waitForBytesWritten(CallbackChannel::IoTimeoutMs)) {
            return refuse("KRDC did not take the link");
        }
    }
    QByteArray reply;
    while (!reply.contains('\n') && reply.size() < 16) {
        if (!socket.bytesAvailable() && !socket.waitForReadyRead(CallbackChannel::IoTimeoutMs)) {
            break;
        }
        reply += socket.read(16 - reply.size());
    }
    if (reply != "OK\n") {
        return refuse("KRDC refused the link; the sign-in failed");
    }
    return 0;
}
