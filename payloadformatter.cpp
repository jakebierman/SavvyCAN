#include "payloadformatter.h"

#include <QStringList>
#include <cstring>

namespace {

quint64 readUnsigned(const QByteArray &payload, int offset, int length, bool littleEndian)
{
    quint64 value = 0;
    for (int i = 0; i < length; ++i)
    {
        const int sourceIndex = littleEndian ? offset + i : offset + length - 1 - i;
        value |= static_cast<quint64>(static_cast<quint8>(payload.at(sourceIndex))) << (i * 8);
    }
    return value;
}

qint64 asSigned(quint64 value, int byteLength)
{
    const int bitCount = byteLength * 8;
    if (bitCount < 64 && (value & (quint64(1) << (bitCount - 1))))
        value |= (~quint64(0) << bitCount);

    qint64 signedValue;
    std::memcpy(&signedValue, &value, sizeof(signedValue));
    return signedValue;
}

} // namespace

bool PayloadFormatter::compile(const QString &format, QString *error, bool repeatSingleField)
{
    const QStringList tokens = format.simplified().split(' ', Qt::SkipEmptyParts);
    QVector<Field> parsedFields;
    int offset = 0;

    if (tokens.isEmpty())
    {
        if (error)
            *error = QStringLiteral("Enter at least one payload type.");
        return false;
    }

    for (const QString &originalToken : tokens)
    {
        const QString token = originalToken.toLower();
        Field field;
        field.offset = offset;

        if (token == QStringLiteral("u8") || token == QStringLiteral("i8"))
        {
            field.type = token.startsWith('u') ? ValueType::Unsigned : ValueType::Signed;
            field.byteLength = 1;
            field.littleEndian = true;
        }
        else
        {
            const bool validPrefix = token.startsWith('u') || token.startsWith('i');
            const bool littleEndian = token.endsWith(QStringLiteral("le"));
            const bool bigEndian = token.endsWith(QStringLiteral("be"));
            bool widthValid = false;
            const int width = token.mid(1, token.length() - 3).toInt(&widthValid);

            if (!validPrefix || (!littleEndian && !bigEndian) || !widthValid ||
                (width != 16 && width != 32 && width != 64))
            {
                if (error)
                    *error = QStringLiteral("Unknown payload type: %1").arg(originalToken);
                return false;
            }

            field.type = token.startsWith('u') ? ValueType::Unsigned : ValueType::Signed;
            field.byteLength = width / 8;
            field.littleEndian = littleEndian;
        }

        parsedFields.append(field);
        offset += field.byteLength;
    }

    fields = parsedFields;
    compiledFormat = tokens.join(' ');
    repeatField = repeatSingleField && fields.size() == 1;
    if (error)
        error->clear();
    return true;
}

QString PayloadFormatter::format(const QByteArray &payload) const
{
    QStringList values;
    if (repeatField)
    {
        const Field &field = fields.first();
        for (int offset = 0; offset < payload.size(); offset += field.byteLength)
        {
            if (offset + field.byteLength > payload.size())
            {
                values.append(QStringLiteral("--"));
                break;
            }

            const quint64 raw = readUnsigned(payload, offset, field.byteLength, field.littleEndian);
            values.append(field.type == ValueType::Signed
                ? QString::number(asSigned(raw, field.byteLength))
                : QString::number(raw));
        }
        return values.join(' ');
    }

    values.reserve(fields.size());

    for (const Field &field : fields)
    {
        if (field.offset + field.byteLength > payload.size())
        {
            values.append(QStringLiteral("--"));
            continue;
        }

        const quint64 raw = readUnsigned(payload, field.offset, field.byteLength, field.littleEndian);
        if (field.type == ValueType::Signed)
            values.append(QString::number(asSigned(raw, field.byteLength)));
        else
            values.append(QString::number(raw));
    }

    return values.join(' ');
}

QString PayloadFormatter::sourceFormat() const
{
    return compiledFormat;
}

bool PayloadFormatter::repeatsSingleField() const
{
    return repeatField;
}
