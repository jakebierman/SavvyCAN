#ifndef DIAGNOSTICGRAPHWINDOW_H
#define DIAGNOSTICGRAPHWINDOW_H

#include <QDialog>
#include <QElapsedTimer>
#include <QMap>

class QCustomPlot;
class QCPGraph;

class DiagnosticGraphWindow : public QDialog
{
public:
    explicit DiagnosticGraphWindow(QWidget *parent = nullptr);
    void addSample(const QString &series, double value);

private:
    QCustomPlot *plot;
    QElapsedTimer elapsed;
    QMap<QString, QCPGraph *> graphs;
};

#endif
