#include "aichattranscript.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

AIChatTranscript::AIChatTranscript(QWidget *parent) :
    QScrollArea(parent),
    content(new QWidget(this)),
    messages(new QVBoxLayout(content))
{
    setWidgetResizable(true);
    setFrameShape(QFrame::StyledPanel);
    messages->setContentsMargins(8, 8, 8, 8);
    messages->setSpacing(8);
    messages->addStretch();
    setWidget(content);
}

void AIChatTranscript::addMessage(const QString &speaker, const QString &text)
{
    const bool user = speaker.compare(tr("You"), Qt::CaseInsensitive) == 0;
    QWidget *row = new QWidget(content);
    QHBoxLayout *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);

    QLabel *bubble = new QLabel(row);
    bubble->setTextFormat(Qt::PlainText);
    bubble->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bubble->setWordWrap(true);
    bubble->setMargin(10);
    bubble->setText(QStringLiteral("%1\n%2").arg(speaker, text));
    bubble->setStyleSheet(user
        ? QStringLiteral("QLabel { background: #1769aa; color: white; border-radius: 8px; }")
        : QStringLiteral(
            "QLabel { background: #d9dde2; color: #111315; "
            "border: 1px solid #aeb4bc; border-radius: 8px; }"));

    if (user) {
        rowLayout->addStretch(1);
        rowLayout->addWidget(bubble);
    } else {
        rowLayout->addWidget(bubble);
        rowLayout->addStretch(1);
    }
    messages->insertWidget(messages->count() - 1, row);
    bubbles.append({bubble, false});
    updateBubbleWidths();
    scrollToBottom();
}

void AIChatTranscript::addSystemMessage(const QString &text)
{
    QLabel *label = new QLabel(text, content);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    label->setMargin(7);
    label->setStyleSheet(QStringLiteral(
        "QLabel { background: #f1f3f4; color: #5f6368; border-radius: 6px; }"));
    messages->insertWidget(messages->count() - 1, label);
    bubbles.append({label, true});
    updateBubbleWidths();
    scrollToBottom();
}

void AIChatTranscript::loadHistory(const QString &history)
{
    clearMessages();
    QString speaker;
    QStringList body;
    auto flush = [&]() {
        if (speaker.isEmpty()) return;
        const QString text = body.join(QLatin1Char('\n')).trimmed();
        if (speaker == QStringLiteral("Application")) addSystemMessage(text);
        else addMessage(speaker == QStringLiteral("User") ? tr("You") : speaker, text);
        body.clear();
    };
    for (const QString &line : history.split(QLatin1Char('\n')))
    {
        const QRegularExpressionMatch match =
            QRegularExpression(QStringLiteral("^(User|Assistant|Application):\\s*(.*)$"))
                .match(line);
        if (match.hasMatch())
        {
            flush();
            speaker = match.captured(1);
            body << match.captured(2);
        }
        else if (!speaker.isEmpty()) body << line;
    }
    flush();
}

void AIChatTranscript::clearMessages()
{
    while (messages->count() > 1)
    {
        QLayoutItem *item = messages->takeAt(0);
        delete item->widget();
        delete item;
    }
    bubbles.clear();
}

void AIChatTranscript::resizeEvent(QResizeEvent *event)
{
    QScrollArea::resizeEvent(event);
    updateBubbleWidths();
}

void AIChatTranscript::updateBubbleWidths()
{
    const int available = qMax(160, viewport()->width() - 28);
    for (const Bubble &bubble : bubbles)
        bubble.label->setMaximumWidth(bubble.system ? available : qMax(140, available * 3 / 4));
}

void AIChatTranscript::scrollToBottom()
{
    QTimer::singleShot(0, this, [this]() {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    });
}
