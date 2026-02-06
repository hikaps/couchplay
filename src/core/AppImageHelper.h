#pragma once

#include <QString>
#include <QObject>
#include <QtQml/qqmlregistration.h>

class AppImageHelper : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit AppImageHelper(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE static bool isRunningAsAppImage();
    Q_INVOKABLE static bool isHelperInstalled();
    Q_INVOKABLE static bool installHelper();
    
private:
    static QString getAppDir();
};
