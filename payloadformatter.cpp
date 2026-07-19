#include "payloadformatter.h"

#include <QStringList>
#include <QRegularExpression>
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

double asFloat(quint64 value, int byteLength)
{
    if (byteLength == 4)
    {
        const quint32 bits = static_cast<quint32>(value);
        float result;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    double result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

QString formatNumber(double value)
{
    return QString::number(value, 'g', 12);
}

} // namespace

QString PayloadFormatter::formatFieldValue(const Field &field, quint64 raw)
{
    const bool masked = field.hasMask;
    if (masked)
        raw &= field.mask;

    if (field.type == ValueType::FloatingPoint)
    {
        double value = asFloat(raw, field.byteLength);
        value = value * field.multiplier / field.divisor + field.additiveOffset;
        return formatNumber(value);
    }

    if (field.hasTransform)
    {
        const double numeric = field.type == ValueType::Signed && !masked
            ? static_cast<double>(asSigned(raw, field.byteLength))
            : static_cast<double>(raw);
        return formatNumber(numeric * field.multiplier / field.divisor + field.additiveOffset);
    }

    if (field.type == ValueType::Signed && !masked)
        return QString::number(asSigned(raw, field.byteLength));
    return QString::number(raw);
}

bool PayloadFormatter::compile(const QString &format, QString *error, bool repeatSingleField)
{
    const QStringList tokens = format.simplified().split(' ', Qt::SkipEmptyParts);
    QVector<Field> parsedFields;
    int offset = 0;
    const QRegularExpression fieldPattern(QStringLiteral(
        "^(?:([A-Za-z_][A-Za-z0-9_]*):)?"
        "(u8|i8|[uif](?:16|32|64)(?:le|be))"
        "(?:@(\\d+))?"
        "(?:&(0[xX][0-9A-Fa-f]+|\\d+))?"
        "(?:\\*([-+]?\\d+(?:\\.\\d+)?))?"
        "(?:/([-+]?\\d+(?:\\.\\d+)?))?"
        "(?:([+-])(\\d+(?:\\.\\d+)?))?$"));

    if (tokens.isEmpty())
    {
        if (error)
            *error = QStringLiteral("Enter at least one payload type.");
        return false;
    }

    for (const QString &originalToken : tokens)
    {
        const QRegularExpressionMatch match = fieldPattern.match(originalToken);
        if (!match.hasMatch())
        {
            if (error)
                *error = QStringLiteral("Invalid payload field: %1").arg(originalToken);
            return false;
        }

        const QString token = match.captured(2).toLower();
        Field field;
        field.name = match.captured(1);
        field.offset = match.captured(3).isEmpty() ? offset : match.captured(3).toInt();

        if (token == QStringLiteral("u8") || token == QStringLiteral("i8"))
        {
            field.type = token.startsWith('u') ? ValueType::Unsigned : ValueType::Signed;
            field.byteLength = 1;
            field.littleEndian = true;
        }
        else
        {
            const bool validPrefix = token.startsWith('u') || token.startsWith('i') || token.startsWith('f');
            const bool littleEndian = token.endsWith(QStringLiteral("le"));
            const bool bigEndian = token.endsWith(QStringLiteral("be"));
            bool widthValid = false;
            const int width = token.mid(1, token.length() - 3).toInt(&widthValid);

            if (!validPrefix || (!littleEndian && !bigEndian) || !widthValid ||
                (width != 16 && width != 32 && width != 64) ||
                (token.startsWith('f') && width != 32 && width != 64))
            {
                if (error)
                    *error = QStringLiteral("Unknown payload type: %1").arg(originalToken);
                return false;
            }

            field.type = token.startsWith('u') ? ValueType::Unsigned
                : token.startsWith('i') ? ValueType::Signed : ValueType::FloatingPoint;
            field.byteLength = width / 8;
            field.littleEndian = littleEndian;
        }

        if (!match.captured(4).isEmpty())
        {
            bool maskValid = false;
            field.mask = match.captured(4).toULongLong(&maskValid, 0);
            if (!maskValid || field.type == ValueType::FloatingPoint)
            {
                if (error)
                    *error = QStringLiteral("Invalid mask in field: %1").arg(originalToken);
                return false;
            }
            field.hasMask = true;
        }

        if (!match.captured(5).isEmpty())
        {
            field.multiplier = match.captured(5).toDouble();
            field.hasTransform = true;
        }
        if (!match.captured(6).isEmpty())
        {
            field.divisor = match.captured(6).toDouble();
            if (field.divisor == 0.0)
            {
                if (error)
                    *error = QStringLiteral("Divisor cannot be zero: %1").arg(originalToken);
                return false;
            }
            field.hasTransform = true;
        }
        if (!match.captured(8).isEmpty())
        {
            field.additiveOffset = match.captured(8).toDouble();
            if (match.captured(7) == QStringLiteral("-"))
                field.additiveOffset = -field.additiveOffset;
            field.hasTransform = true;
        }

        parsedFields.append(field);
        offset = field.offset + field.byteLength;
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
            values.append(formatFieldValue(field, raw));
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
        QString value = formatFieldValue(field, raw);
        if (!field.name.isEmpty())
            value.prepend(field.name + '=');
        values.append(value);
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
