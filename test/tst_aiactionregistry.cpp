#include "tst_aiactionregistry.h"

#include <QJsonDocument>
#include <QtTest>

#include "re/aiactionregistry.h"

void TestAIActionRegistry::routesEvaluationPrompts()
{
    const QJsonObject diagnostics = AIActionRegistry::skillDiagnostics();
    QCOMPARE(diagnostics.value(QStringLiteral("evaluations_total")).toInt(), 20);
    QCOMPARE(diagnostics.value(QStringLiteral("evaluations_passed")).toInt(), 20);
    QVERIFY2(diagnostics.value(QStringLiteral("evaluation_failures")).toArray().isEmpty(),
             qPrintable(QString::fromUtf8(
                 QJsonDocument(diagnostics).toJson(QJsonDocument::Indented))));
}

void TestAIActionRegistry::coversEveryCapability()
{
    const QJsonObject diagnostics = AIActionRegistry::skillDiagnostics();
    const QByteArray report = QJsonDocument(diagnostics).toJson(QJsonDocument::Indented);
    QVERIFY2(diagnostics.value(QStringLiteral("uncovered_capabilities")).toArray().isEmpty(),
             report.constData());
    QVERIFY2(diagnostics.value(
        QStringLiteral("unknown_capability_references")).toArray().isEmpty(),
             report.constData());
    QVERIFY2(diagnostics.value(QStringLiteral("valid")).toBool(), report.constData());
}

void TestAIActionRegistry::combinesDbcAndReverseEngineering()
{
    const QStringList selected = AIActionRegistry::matchingSkills(
        QStringLiteral("use sniffer evidence to add an enum bitfield to the DBC"),
        QJsonObject{{QStringLiteral("active_workspace"), QStringLiteral("CAN Sniffer")}});
    QVERIFY(selected.contains(QStringLiteral("dbc_signals")));
    QVERIFY(selected.contains(QStringLiteral("reverse_engineering")));
}
