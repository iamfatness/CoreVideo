#include "mainwindow.h"
#include "sidecar-style.h"
#include "show-theme.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QStyleFactory>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("CoreVideo Sidecar");
    app.setOrganizationName("CoreVideo");
    app.setApplicationVersion("0.1.2");

    // Fusion gives clean cross-platform rendering with full QSS support
    app.setStyle(QStyleFactory::create("Fusion"));
    const auto builtins = ShowTheme::builtIns();
    app.setStyleSheet(sidecar_stylesheet(&builtins.first())); // Midnight Blue default

    QCommandLineParser parser;
    parser.setApplicationDescription("CoreVideo Sidecar — broadcast control surface.");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption hostOpt("obs-host",
        "Override OBS WebSocket host.", "host");
    QCommandLineOption portOpt("obs-port",
        "Override OBS WebSocket port.", "port");
    QCommandLineOption passOpt("obs-password",
        "Override OBS WebSocket password.", "password");
    QCommandLineOption autoOpt("obs-autoconnect",
        "Connect to OBS automatically on launch.");

    parser.addOption(hostOpt);
    parser.addOption(portOpt);
    parser.addOption(passOpt);
    parser.addOption(autoOpt);
    parser.process(app);

    MainWindow::StartupConfig startup;
    if (parser.isSet(hostOpt)) {
        startup.hostOverride = parser.value(hostOpt);
    }
    if (parser.isSet(portOpt)) {
        bool ok = false;
        const int p = parser.value(portOpt).toInt(&ok);
        if (ok && p > 0 && p < 65536) startup.portOverride = p;
    }
    if (parser.isSet(passOpt)) {
        startup.passwordOverride = parser.value(passOpt);
    }
    startup.autoConnect = parser.isSet(autoOpt);

    MainWindow w(startup);
    w.show();

    return app.exec();
}
