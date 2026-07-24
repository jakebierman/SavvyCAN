#include "diagnosticgraphwindow.h"

#include "qcustomplot.h"

#include <QPushButton>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QVBoxLayout>

DiagnosticGraphWindow::DiagnosticGraphWindow(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Diagnostic Value Graph"));
    resize(900, 520);
    QVBoxLayout *layout = new QVBoxLayout(this);
    plot = new QCustomPlot(this);
    plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    plot->legend->setVisible(true);
    plot->xAxis->setLabel(tr("Elapsed time (s)"));
    layout->addWidget(plot);
    QHBoxLayout *buttons = new QHBoxLayout;
    QPushButton *exportImage = new QPushButton(tr("Export PNG"), this);
    QPushButton *clear = new QPushButton(tr("Clear"), this);
    buttons->addStretch();
    buttons->addWidget(exportImage);
    buttons->addWidget(clear);
    layout->addLayout(buttons);
    connect(exportImage, &QPushButton::clicked, this, [this]() {
        QString fileName = QFileDialog::getSaveFileName(this, tr("Export diagnostic graph"), QString(),
                                                         tr("PNG images (*.png)"));
        if (fileName.isEmpty()) return;
        if (!fileName.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) fileName += QStringLiteral(".png");
        plot->savePng(fileName, 0, 0, 1.5, -1);
    });
    connect(clear, &QPushButton::clicked, this, [this]() {
        for (QCPGraph *graph : graphs) graph->data()->clear();
        elapsed.restart();
        plot->replot();
    });
    elapsed.start();
}

void DiagnosticGraphWindow::addSample(const QString &series, double value)
{
    QCPGraph *graph = graphs.value(series, nullptr);
    if (!graph)
    {
        graph = plot->addGraph();
        graph->setName(series);
        graph->setPen(QPen(QColor::fromHsv((graphs.size() * 67) % 360, 190, 210), 2));
        graphs.insert(series, graph);
    }
    graph->addData(elapsed.elapsed() / 1000.0, value);
    plot->rescaleAxes(true);
    plot->replot(QCustomPlot::rpQueuedReplot);
}
