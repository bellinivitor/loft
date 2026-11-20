// loft — local PHP and Node sites at name.test, served by nginx, for Omarchy.

#include <QGuiApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "backend.h"
#include "cli.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // Commands run without a display, so they work over SSH, from systemd
    // units, and from agents.
    const bool window = wantsWindow(argc, argv);
    if (!window)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    app.setApplicationName("loft");
    app.setApplicationVersion("0.1.0");

    // Matches the window to loft.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("loft");
    app.setWindowIcon(QIcon::fromTheme("loft"));

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    if (!window)
        return runCli(app.arguments());

    // One window: `loft app sites` from the bar hands its page to a window
    // that's already open and leaves, rather than opening a second one.
    const QString page = app.arguments().value(2);
    const QString instance = QString("loft-window-%1").arg(qEnvironmentVariable("USER"));
    {
        QLocalSocket running;
        running.connectToServer(instance);
        if (running.waitForConnected(300)) {
            running.write(page.toUtf8() + "\n");
            running.waitForBytesWritten(300);
            return 0;
        }
    }
    QLocalServer::removeServer(instance);
    QLocalServer server;
    server.listen(instance);

    QQuickStyle::setStyle("Material");

    Theme theme;
    Backend backend;

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    if (!page.isEmpty())
        backend.activate(page);
    QObject::connect(&server, &QLocalServer::newConnection, &backend, [&server, &backend] {
        QLocalSocket *client = server.nextPendingConnection();
        QObject::connect(client, &QLocalSocket::readyRead, client, [client, &backend] {
            backend.activate(QString::fromUtf8(client->readLine()).trimmed());
            client->disconnectFromServer();
        });
        QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
    });

    return app.exec();
}
