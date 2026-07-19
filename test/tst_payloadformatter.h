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
    void formatsNamedCalculatedFields();
    void supportsOffsetsMasksAndFloats();
    void supportsShiftsUnitsPrecisionAndWarnings();
    void marksMissingData();
    void rejectsInvalidFormats();
    void routesFormatsByBusIdAndFrameType();
    void restoresAssignmentMapFromSettings();
};

#endif // TST_PAYLOADFORMATTER_H
