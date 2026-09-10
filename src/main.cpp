#include "mainwindow.h"

#include <QApplication>
#include <QFont>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("生产助手"));
    application.setOrganizationName(QStringLiteral("ProductionAssistant"));
    application.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));

    MainWindow window;
    window.show();
    return application.exec();
}

