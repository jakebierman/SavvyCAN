#ifndef TST_PAYLOADFORMATTER_H
#define TST_PAYLOADFORMATTER_H

#include <QObject>

class TestPayloadFormatter : public QObject
{
    Q_OBJECT

private slots:
    void formatsIntegerTypes();
    void supportsSequentialFields();
    void repeatsPresetAcrossPayload();
    void formats64BitValues();
    void marksMissingData();
    void rejectsInvalidFormats();
};

#endif // TST_PAYLOADFORMATTER_H
