#include "mainwindow.h"
#include "sidecar-style.h"
#include "show-theme.h"
#include <QApplication>
#include <QStyleFactory>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("CoreVideo Sidecar");
    app.setOrganizationName("CoreVideo");
    app.setApplicationVersion("0.1.0");

    // Fusion gives clean cross-platform rendering with full QSS support
    app.setStyle(QStyleFactory::create("Fusion"));
    const auto builtins = ShowTheme::builtIns();
    app.setStyleSheet(sidecar_stylesheet(&builtins.first())); // Midnight Blue default

    MainWindow w;
    w.show();

    return app.exec();
}
