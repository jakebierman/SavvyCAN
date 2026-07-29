#ifndef AIACTIONREGISTRY_H
#define AIACTIONREGISTRY_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

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
    static QJsonArray skills();
    static QStringList matchingSkills(const QString &question,
                                      const QJsonObject &applicationContext = QJsonObject());
    static QJsonArray catalogForQuestion(
        const QString &question,
        const QJsonObject &applicationContext = QJsonObject());
    static QString skillVersion();
    static QString skillContext(
        const QString &question,
        const QJsonObject &applicationContext = QJsonObject());
    static QJsonObject skillDiagnostics(
        const QJsonObject &applicationContext = QJsonObject());
    static Risk risk(const QJsonObject &definition);
    static bool validate(const QJsonObject &action, QString *error);
};

#endif
