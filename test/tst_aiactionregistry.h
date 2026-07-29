#ifndef TST_AIACTIONREGISTRY_H
#define TST_AIACTIONREGISTRY_H

#include <QObject>

class TestAIActionRegistry : public QObject
{
    Q_OBJECT

private slots:
    void routesEvaluationPrompts();
    void coversEveryCapability();
    void combinesDbcAndReverseEngineering();
};

#endif
