/*
    SPDX-FileCopyrightText: 2026 KRDC contributors
    SPDX-License-Identifier: MIT
*/
// Test only, never shipped: KRDC's own Avd::CallbackReceiver waiting for one sign-in, run by
// test-sandbox-callback.sh inside a sandbox of the installed app.
//   probe REDIRECT_URI STATE  ->  READY, then RECEIVED <code> (exit 0) or FAILED (exit 1)

#include "avd/avdcallback.h"

#include <QCoreApplication>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 3) {
        return 2;
    }
    Avd::CallbackReceiver receiver(args.at(1), args.at(2));
    receiver.setTimeout(std::chrono::seconds(90));
    QObject::connect(&receiver, &Avd::CallbackReceiver::received, &app, [&app](const Avd::AuthorizationCallback &callback) {
        std::printf("RECEIVED %s\n", qPrintable(callback.code));
        std::fflush(stdout);
        app.exit(0);
    });
    QObject::connect(&receiver, &Avd::CallbackReceiver::failed, &app, [&app](const QString &message) {
        std::printf("FAILED %s\n", qPrintable(message));
        std::fflush(stdout);
        app.exit(1);
    });
    QString error;
    if (!receiver.listen(&error)) {
        std::printf("FAILED %s\n", qPrintable(error));
        return 1;
    }
    std::printf("READY\n");
    std::fflush(stdout);
    return app.exec();
}
