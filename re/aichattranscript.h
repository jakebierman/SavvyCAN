#ifndef AICHATTRANSCRIPT_H
#define AICHATTRANSCRIPT_H

#include <QScrollArea>

class QLabel;
class QVBoxLayout;

class AIChatTranscript : public QScrollArea
{
public:
    explicit AIChatTranscript(QWidget *parent = nullptr);

    void addMessage(const QString &speaker, const QString &text);
    void addSystemMessage(const QString &text);
    void loadHistory(const QString &history);
    void clearMessages();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Bubble {
        QLabel *label;
        bool system;
    };

    void updateBubbleWidths();
    void scrollToBottom();

    QWidget *content;
    QVBoxLayout *messages;
    QVector<Bubble> bubbles;
};

#endif
