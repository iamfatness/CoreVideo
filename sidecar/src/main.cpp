#include "mainwindow.h"
#include "sidecar-style.h"
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
    app.setStyleSheet(sidecar_stylesheet());

    MainWindow w;
    w.show();

    return app.exec();
}
