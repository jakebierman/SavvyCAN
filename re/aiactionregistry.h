#ifndef AIACTIONREGISTRY_H
#define AIACTIONREGISTRY_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class AIActionRegistry
{
public:
    enum Risk {
        ReadOnly,
        Edit,
        ConfirmSend,
        ArmedConfirmSend
    };

    static QJsonArray catalog();
    static QJsonObject definition(const QString &capability);
    static QString catalogText();
    static Risk risk(const QJsonObject &definition);
    static bool validate(const QJsonObject &action, QString *error);
};

#endif
