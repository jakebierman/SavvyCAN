#include <QtTest>

#include "payloadformatter.h"
#include "tst_payloadformatter.h"

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
    QCOMPARE(error, QString("Unknown payload type: u24le"));
}
