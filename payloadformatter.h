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
        Signed
    };

    struct Field
    {
        ValueType type;
        int byteLength;
        bool littleEndian;
        int offset;
    };

    QVector<Field> fields;
    QString compiledFormat;
    bool repeatField = false;
};

#endif // PAYLOADFORMATTER_H
