#include <QtTest>

#include "payloadformatter.h"
#include "payloadformatrouter.h"
#include "tst_payloadformatter.h"

#include <QSettings>
#include <QTemporaryDir>

void TestPayloadFormatter::formatsIntegerTypes()
{
    PayloadFormatter formatter;
    QVERIFY(formatter.compile("u16le u16be i16le i16be u32le i32be"));

    const QByteArray payload = QByteArray::fromHex(
        "34121234FEFFFFFE78563412FFFFFFFE");
    QCOMPARE(formatter.format(payload),
             QString("4660 4660 -2 -2 305419896 -2"));
}

void TestPayloadFormatter::supportsSequentialFields()
{
    PayloadFormatter formatter;
    QVERIFY(formatter.compile("u8 i8 u16le i32le"));

    QCOMPARE(formatter.format(QByteArray::fromHex("7FFF341200000080")),
             QString("127 -1 4660 -2147483648"));
}

void TestPayloadFormatter::repeatsPresetAcrossPayload()
{
    PayloadFormatter formatter;
    QVERIFY(formatter.compile("u16le", nullptr, true));

    QCOMPARE(formatter.format(QByteArray::fromHex("0100FF7F00803412")),
             QString("1 32767 32768 4660"));
}

void TestPayloadFormatter::formats64BitValues()
{
    PayloadFormatter formatter;
    QVERIFY(formatter.compile("u64be i64le"));

    QCOMPARE(formatter.format(QByteArray::fromHex(
                 "FFFFFFFFFFFFFFFF0000000000000080")),
             QString("18446744073709551615 -9223372036854775808"));
}

void TestPayloadFormatter::formatsNamedCalculatedFields()
{
    PayloadFormatter formatter;
    QVERIFY(formatter.compile("rpm:u16be*0.25 temp:i8-40 voltage:u16le/10"));

    QCOMPARE(formatter.format(QByteArray::fromHex("1C84C87405")),
             QString("rpm=1825 temp=-96 voltage=139.6"));
    const QVector<PayloadFormatter::FormattedField> fields =
        formatter.formatFields(QByteArray::fromHex("1C84C87405"));
    QCOMPARE(fields.size(), 3);
    QCOMPARE(fields.at(0).name, QString("rpm"));
    QCOMPARE(fields.at(0).value, QString("1825"));
}

void TestPayloadFormatter::formatsFixedLengthStrings()
{
    PayloadFormatter formatter;
    QVERIFY(formatter.compile("vin:ascii17 label:utf88 serial:str6@30"));
    QByteArray payload("WVWZZZ1JZXW000001", 17);
    payload.append(QString::fromUtf8("Grüße").toUtf8().leftJustified(8, '\0'));
    payload.append(QByteArray(5, '\0'));
    payload.append("ABC123", 6);
    QCOMPARE(formatter.format(payload), QString::fromUtf8("vin=WVWZZZ1JZXW000001 label=Grüße serial=ABC123"));
    QCOMPARE(formatter.validationWarnings(32),
             QStringList({QString("serial requires bytes 30-35, but payload has 32 bytes")}));
}

void TestPayloadFormatter::supportsOffsetsMasksAndFloats()
{
    PayloadFormatter formatter;
    QVERIFY(formatter.compile("flags:u16be@1&0x07FF single:f32le@3 precise:f64be@7"));

    QCOMPARE(formatter.format(QByteArray::fromHex(
                 "AAFFFF0000C03F4004000000000000")),
             QString("flags=2047 single=1.5 precise=2.5"));
}

void TestPayloadFormatter::supportsShiftsUnitsPrecisionAndWarnings()
{
    PayloadFormatter formatter;
    QVERIFY(formatter.compile("gear:u16be@0&0x0700>>8 temp:i8@2*0.5-40[C]{1}"));

    QCOMPARE(formatter.format(QByteArray::fromHex("050064")),
             QString("gear=5 temp=10.0 C"));
    QCOMPARE(formatter.validationWarnings(2),
             QStringList({QString("temp requires bytes 2-2, but payload has 2 bytes")}));
}

void TestPayloadFormatter::formatsEnumValues()
{
    PayloadFormatter formatter;
    QVERIFY(formatter.compile(
        "regen:u8@0&0x01{0:Inactive,1:Active} "
        "type:u8@0&0x02>>1{0:Passive,1:Active} "
        "state:u8@1{0:Off,1:On,*:Unknown}"));

    QCOMPARE(formatter.format(QByteArray::fromHex("0307")),
             QString("regen=Active type=Active state=Unknown"));

    QVERIFY(formatter.compile("state:u8{0:Off,1:On}"));
    QCOMPARE(formatter.format(QByteArray::fromHex("02")), QString("state=2"));

    QString error;
    QVERIFY(!formatter.compile("state:u8{1:On,1:AlsoOn}", &error));
    QVERIFY(error.contains("duplicate"));
    QVERIFY(!formatter.compile("scaled:u8*2{2:Two}", &error));
    QVERIFY(error.contains("transforms"));
}

void TestPayloadFormatter::marksMissingData()
{
    PayloadFormatter formatter;
    QVERIFY(formatter.compile("u16le u32le u8"));

    QCOMPARE(formatter.format(QByteArray::fromHex("010203")), QString("513 -- --"));
}

void TestPayloadFormatter::rejectsInvalidFormats()
{
    PayloadFormatter formatter;
    QString error;

    QVERIFY(!formatter.compile("", &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!formatter.compile("u24le", &error));
    QCOMPARE(error, QString("Invalid payload field: u24le"));
    QVERIFY(!formatter.compile("value:u16le/0", &error));
    QVERIFY(error.contains("zero"));
}

void TestPayloadFormatter::routesFormatsByBusIdAndFrameType()
{
    PayloadFormatRouter router;
    QVERIFY(router.setFormat(0, 0x123, false, "bus0:u16be"));
    QVERIFY(router.setFormat(1, 0x123, false, "bus1:u16le"));
    QVERIFY(router.setFormat(0, 0x123, true, "extended:u8"));

    CANFrame frame;
    frame.setFrameId(0x123);
    frame.setPayload(QByteArray::fromHex("0102"));
    frame.bus = 0;
    frame.setExtendedFrameFormat(false);
    QCOMPARE(router.formatterFor(frame)->format(frame.payload()), QString("bus0=258"));
    frame.bus = 1;
    QCOMPARE(router.formatterFor(frame)->format(frame.payload()), QString("bus1=513"));
    frame.bus = 0;
    frame.setExtendedFrameFormat(true);
    QCOMPARE(router.formatterFor(frame)->format(frame.payload()), QString("extended=1"));

    frame.setFrameId(0x456);
    QVERIFY(router.formatterFor(frame) == nullptr);
    QVERIFY(router.setFormat(-1, 0x456, false, "legacy:u8"));
    QCOMPARE(router.formatterFor(frame)->format(frame.payload()), QString("legacy=1"));
}

void TestPayloadFormatter::restoresAssignmentMapFromSettings()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString key = PayloadFormatRouter::key(2, 0x18DAF110, true);
    {
        QSettings settings(directory.filePath("payload.ini"), QSettings::IniFormat);
        QVariantMap assignments;
        assignments.insert(key, QStringLiteral("rpm:u16be@0*0.25[rpm]{0}"));
        settings.setValue("Main/PayloadIdFormats", assignments);
    }

    QSettings restored(directory.filePath("payload.ini"), QSettings::IniFormat);
    const QVariantMap assignments = restored.value("Main/PayloadIdFormats").toMap();
    QCOMPARE(assignments.value(key).toString(), QString("rpm:u16be@0*0.25[rpm]{0}"));

    PayloadFormatRouter router;
    const QStringList parts = key.split(':');
    QVERIFY(router.setFormat(parts.at(0).toInt(), parts.at(1).toUInt(),
                             parts.at(2).toInt() != 0, assignments.value(key).toString()));
    CANFrame frame;
    frame.bus = 2;
    frame.setFrameId(0x18DAF110);
    frame.setExtendedFrameFormat(true);
    frame.setPayload(QByteArray::fromHex("1C84"));
    QCOMPARE(router.formatterFor(frame)->format(frame.payload()), QString("rpm=1825 rpm"));
}
