#include <QCoreApplication>
#include <QtTest>

#include "tst_aiactionregistry.h"

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    TestAIActionRegistry test;
    return QTest::qExec(&test, argc, argv);
}
