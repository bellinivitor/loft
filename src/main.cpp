// loft — local PHP and Node sites at name.test, served by nginx, for Omarchy.

#include <QGuiApplication>
#include <QIcon>
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

    return app.exec();
}
