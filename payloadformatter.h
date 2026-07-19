#ifndef PAYLOADFORMATTER_H
#define PAYLOADFORMATTER_H

#include <QByteArray>
#include <QString>
#include <QVector>

enum class PayloadDisplayMode
{
    RawHex,
    RawDecimal,
    Typed
};

class PayloadFormatter
{
public:
    bool compile(const QString &format, QString *error = nullptr, bool repeatSingleField = false);
    QString format(const QByteArray &payload) const;
    QString sourceFormat() const;
    bool repeatsSingleField() const;

private:
    enum class ValueType
    {
        Unsigned,
        Signed,
        FloatingPoint
    };

    struct Field
    {
        ValueType type;
        int byteLength;
        bool littleEndian;
        int offset;
        QString name;
        bool hasMask = false;
        quint64 mask = 0;
        double multiplier = 1.0;
        double divisor = 1.0;
        double additiveOffset = 0.0;
        bool hasTransform = false;
    };

    QVector<Field> fields;
    QString compiledFormat;
    bool repeatField = false;
    static QString formatFieldValue(const Field &field, quint64 raw);
};

#endif // PAYLOADFORMATTER_H
