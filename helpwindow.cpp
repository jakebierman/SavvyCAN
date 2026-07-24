#include <QtDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextCursor>
#include "utility.h"
#include "helpwindow.h"
#include "ui_helpwindow.h"

HelpWindow* HelpWindow::self = nullptr;

HelpWindow::HelpWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::HelpWindow)
{
    ui->setupUi(this);
    setWindowFlags(Qt::Window);

    readSettings();
}

HelpWindow::~HelpWindow()
{
    writeSettings();
    delete ui;
}

void HelpWindow::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event);
    writeSettings();
}

void HelpWindow::readSettings()
{
    QSettings settings;
    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        resize(settings.value("HelpViewer/WindowSize", QSize(600, 700)).toSize());
        move(Utility::constrainedWindowPos(settings.value("HelpViewer/WindowPos", QPoint(50, 50)).toPoint()));
    }
}

void HelpWindow::writeSettings()
{
    QSettings settings;

    if (settings.value("Main/SaveRestorePositions", false).toBool())
    {
        settings.setValue("HelpViewer/WindowSize", size());
        settings.setValue("HelpViewer/WindowPos", pos());
    }
}

HelpWindow* HelpWindow::getRef()
{
    if (!self)
    {
        self = new HelpWindow();
    }

    return self;
}

void HelpWindow::showHelp(QString help)
{
    const QString applicationDir = QCoreApplication::applicationDirPath();
    const QStringList roots = {
        applicationDir + QStringLiteral("/help"),
        QDir(applicationDir).absoluteFilePath(QStringLiteral("../help")),
        QDir(applicationDir).absoluteFilePath(QStringLiteral("../../help")),
        QDir::current().absoluteFilePath(QStringLiteral("help"))
    };
    QString helpfile;
    for (const QString &root : roots)
    {
        const QString candidate = QDir(root).absoluteFilePath(help);
        if (QFileInfo::exists(candidate)) { helpfile = candidate; break; }
    }
    if (helpfile.isEmpty())
    {
        ui->textHelp->setHtml(tr("<h1>Help page unavailable</h1>"
            "<p>Could not find <code>%1</code> in the installed or source-tree help directories.</p>")
            .arg(help.toHtmlEscaped()));
    }
    else
    {
        QFile file(helpfile);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            ui->textHelp->document()->setBaseUrl(
                QUrl::fromLocalFile(QFileInfo(helpfile).absolutePath() + QLatin1Char('/')));
            ui->textHelp->setMarkdown(QString::fromUtf8(file.readAll()));
            ui->textHelp->moveCursor(QTextCursor::Start);
        }
        else ui->textHelp->setPlainText(tr("Could not open help file: %1").arg(helpfile));
    }

    readSettings();
    self->show();
    self->raise();
    self->activateWindow();
}
