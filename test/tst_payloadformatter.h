#ifndef TST_PAYLOADFORMATTER_H
#define TST_PAYLOADFORMATTER_H

#include <QObject>

class TestPayloadFormatter : public QObject
{
    Q_OBJECT

private slots:
    void formatsBitFields();
    void formatsIntegerTypes();
    void supportsSequentialFields();
    void repeatsPresetAcrossPayload();
    void formats64BitValues();
    void formatsNamedCalculatedFields();
    void formatsFixedLengthStrings();
    void supportsOffsetsMasksAndFloats();
    void supportsShiftsUnitsPrecisionAndWarnings();
    void formatsEnumValues();
    void marksMissingData();
    void rejectsInvalidFormats();
    void routesFormatsByBusIdAndFrameType();
    void restoresAssignmentMapFromSettings();
};

#endif // TST_PAYLOADFORMATTER_H
