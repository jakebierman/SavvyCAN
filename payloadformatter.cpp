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

QString PayloadFormatter::formatFieldValue(const Field &field, quint64 raw, bool includeUnit)
{
    const bool masked = field.hasMask;
    if (masked)
        raw &= field.mask;
    if (field.shift > 0)
        raw >>= field.shift;
    else if (field.shift < 0)
        raw <<= -field.shift;

    if (!field.enumValues.isEmpty() || field.hasEnumFallback)
    {
        const quint64 enumKey = field.type == ValueType::Signed && !masked
            ? static_cast<quint64>(asSigned(raw, field.byteLength)) : raw;
        const auto mapped = field.enumValues.constFind(enumKey);
        if (mapped != field.enumValues.constEnd()) return mapped.value();
        if (field.hasEnumFallback) return field.enumFallback;
    }

    if (field.type == ValueType::Bit)
    {
        if (field.bitDisplay == BitDisplay::Boolean)
            return raw ? QStringLiteral("True") : QStringLiteral("False");
        if (field.bitDisplay == BitDisplay::Visual)
            return raw ? QStringLiteral("[X]") : QStringLiteral("[ ]");
        return QString::number(raw);
    }

    if (field.type == ValueType::FloatingPoint)
    {
        double value = asFloat(raw, field.byteLength);
        value = value * field.multiplier / field.divisor + field.additiveOffset;
        QString result = field.precision >= 0
            ? QString::number(value, 'f', field.precision) : formatNumber(value);
        if (includeUnit && !field.unit.isEmpty()) result += ' ' + field.unit;
        return result;
    }

    if (field.hasTransform)
    {
        const double numeric = field.type == ValueType::Signed && !masked
            ? static_cast<double>(asSigned(raw, field.byteLength))
            : static_cast<double>(raw);
    {
        QString result = field.precision >= 0
            ? QString::number(numeric * field.multiplier / field.divisor + field.additiveOffset,
                              'f', field.precision)
            : formatNumber(numeric * field.multiplier / field.divisor + field.additiveOffset);
        if (includeUnit && !field.unit.isEmpty()) result += ' ' + field.unit;
        return result;
    }
    }

    QString result = field.type == ValueType::Signed && !masked
        ? QString::number(asSigned(raw, field.byteLength)) : QString::number(raw);
    if (includeUnit && !field.unit.isEmpty()) result += ' ' + field.unit;
    return result;
}

QString PayloadFormatter::formatTextValue(const Field &field, const QByteArray &payload, int offset)
{
    QByteArray bytes = payload.mid(offset, field.byteLength);
    const int terminator = bytes.indexOf('\0');
    if (terminator >= 0) bytes.truncate(terminator);
    return field.type == ValueType::AsciiText
        ? QString::fromLatin1(bytes) : QString::fromUtf8(bytes);
}

bool PayloadFormatter::compile(const QString &format, QString *error, bool repeatSingleField)
{
    const QStringList tokens = format.simplified().split(' ', Qt::SkipEmptyParts);
    QVector<Field> parsedFields;
    int offset = 0;
    const QRegularExpression fieldPattern(QStringLiteral(
        "^(?:([A-Za-z_][A-Za-z0-9_]*):)?"
        "(u8|i8|(?:bit|bool|flag)[0-7]|[uif](?:16|32|64)(?:le|be)|(?:ascii|utf8|str)\\d+)"
        "(?:@(\\d+))?"
        "(?:&(0[xX][0-9A-Fa-f]+|\\d+))?"
        "(?:(>>|<<)(\\d+))?"
        "(?:\\*([-+]?\\d+(?:\\.\\d+)?))?"
        "(?:/([-+]?\\d+(?:\\.\\d+)?))?"
        "(?:([+-])(\\d+(?:\\.\\d+)?))?"
        "(?:\\[([^\\]]+)\\])?"
        "(?:\\{(\\d+)\\})?"
        "(?:\\{([^{}]*:[^{}]*)\\})?$"));

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

        if (token.startsWith(QStringLiteral("ascii")) || token.startsWith(QStringLiteral("utf8")) ||
            token.startsWith(QStringLiteral("str")))
        {
            const int prefixLength = token.startsWith(QStringLiteral("ascii")) ? 5
                : token.startsWith(QStringLiteral("utf8")) ? 4 : 3;
            bool lengthValid = false;
            field.byteLength = token.mid(prefixLength).toInt(&lengthValid);
            if (!lengthValid || field.byteLength < 1)
            {
                if (error) *error = QStringLiteral("Invalid string length: %1").arg(originalToken);
                return false;
            }
            field.type = token.startsWith(QStringLiteral("ascii"))
                ? ValueType::AsciiText : ValueType::Utf8Text;
            field.littleEndian = false;
        }
        else if (token.startsWith(QStringLiteral("bit")) ||
                 token.startsWith(QStringLiteral("bool")) ||
                 token.startsWith(QStringLiteral("flag")))
        {
            field.type = ValueType::Bit;
            field.byteLength = 1;
            field.littleEndian = true;
            field.hasMask = true;
            field.shift = token.right(1).toInt();
            field.mask = quint64(1) << field.shift;
            field.bitDisplay = token.startsWith(QStringLiteral("bool")) ? BitDisplay::Boolean
                : token.startsWith(QStringLiteral("flag")) ? BitDisplay::Visual
                : BitDisplay::Numeric;
        }
        else if (token == QStringLiteral("u8") || token == QStringLiteral("i8"))
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
            if (field.type == ValueType::Bit)
            {
                if (error) *error = QStringLiteral("Bit fields already select a bit: %1").arg(originalToken);
                return false;
            }
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

        if (!match.captured(6).isEmpty())
        {
            if (field.type == ValueType::Bit)
            {
                if (error) *error = QStringLiteral("Bit fields do not support shifts: %1").arg(originalToken);
                return false;
            }
            const int shiftAmount = match.captured(6).toInt();
            if (shiftAmount >= field.byteLength * 8)
            {
                if (error)
                    *error = QStringLiteral("Shift exceeds field width: %1").arg(originalToken);
                return false;
            }
            field.shift = match.captured(5) == QStringLiteral(">>") ? shiftAmount : -shiftAmount;
        }

        if (!match.captured(7).isEmpty())
        {
            field.multiplier = match.captured(7).toDouble();
            field.hasTransform = true;
        }
        if (!match.captured(8).isEmpty())
        {
            field.divisor = match.captured(8).toDouble();
            if (field.divisor == 0.0)
            {
                if (error)
                    *error = QStringLiteral("Divisor cannot be zero: %1").arg(originalToken);
                return false;
            }
            field.hasTransform = true;
        }
        if (!match.captured(10).isEmpty())
        {
            field.additiveOffset = match.captured(10).toDouble();
            if (match.captured(9) == QStringLiteral("-"))
                field.additiveOffset = -field.additiveOffset;
            field.hasTransform = true;
        }
        field.unit = match.captured(11);
        if (!match.captured(12).isEmpty())
        {
            field.precision = match.captured(12).toInt();
            field.hasTransform = true;
        }

        if (!match.captured(13).isEmpty())
        {
            const QStringList entries = match.captured(13).split(',', Qt::KeepEmptyParts);
            for (const QString &entry : entries)
            {
                const int separator = entry.indexOf(':');
                const QString key = entry.left(separator).trimmed();
                const QString label = entry.mid(separator + 1).trimmed();
                if (separator < 1 || label.isEmpty())
                {
                    if (error) *error = QStringLiteral("Invalid enum entry in field: %1").arg(originalToken);
                    return false;
                }
                if (key == QStringLiteral("*"))
                {
                    if (field.hasEnumFallback)
                    {
                        if (error) *error = QStringLiteral("Duplicate enum fallback in field: %1").arg(originalToken);
                        return false;
                    }
                    field.enumFallback = label;
                    field.hasEnumFallback = true;
                    continue;
                }
                bool keyValid = false;
                quint64 value = key.toULongLong(&keyValid, 0);
                if (!keyValid && key.startsWith('-'))
                    value = static_cast<quint64>(key.toLongLong(&keyValid, 0));
                if (!keyValid || field.enumValues.contains(value))
                {
                    if (error) *error = QStringLiteral("Invalid or duplicate enum value in field: %1").arg(originalToken);
                    return false;
                }
                field.enumValues.insert(value, label);
            }
        }

        if ((field.type == ValueType::AsciiText || field.type == ValueType::Utf8Text) &&
            (field.hasMask || field.shift != 0 || field.hasTransform || !field.unit.isEmpty() ||
             !field.enumValues.isEmpty() || field.hasEnumFallback))
        {
            if (error) *error = QStringLiteral("String fields do not support numeric modifiers: %1").arg(originalToken);
            return false;
        }

        if ((!field.enumValues.isEmpty() || field.hasEnumFallback) &&
            (field.type == ValueType::FloatingPoint || field.hasTransform))
        {
            if (error) *error = QStringLiteral("Enum fields do not support numeric transforms: %1").arg(originalToken);
            return false;
        }

        if (field.type == ValueType::Bit &&
            (field.hasTransform || !field.unit.isEmpty() || field.precision >= 0))
        {
            if (error) *error = QStringLiteral("Bit fields do not support calculations, units, or precision: %1")
                .arg(originalToken);
            return false;
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
    const QVector<FormattedField> formatted = formatFields(payload);
    QStringList values;
    for (int i = 0; i < formatted.size(); ++i)
    {
        QString value = formatted.at(i).value;
        if (!formatted.at(i).unit.isEmpty()) value += ' ' + formatted.at(i).unit;
        if (!repeatField && i < fields.size() && !fields.at(i).name.isEmpty())
            value.prepend(formatted.at(i).name + '=');
        values.append(value);
    }
    return values.join(' ');
}

QVector<PayloadFormatter::FormattedField> PayloadFormatter::formatFields(const QByteArray &payload) const
{
    QVector<FormattedField> values;
    if (repeatField)
    {
        const Field &field = fields.first();
        int fieldIndex = 0;
        for (int offset = 0; offset < payload.size(); offset += field.byteLength)
        {
            if (offset + field.byteLength > payload.size())
            {
                values.append({QStringLiteral("field%1").arg(fieldIndex + 1), QStringLiteral("--"), field.unit});
                break;
            }

            values.append({QStringLiteral("field%1").arg(fieldIndex + 1),
                           field.type == ValueType::AsciiText || field.type == ValueType::Utf8Text
                               ? formatTextValue(field, payload, offset)
                               : formatFieldValue(field, readUnsigned(payload, offset, field.byteLength,
                                                                      field.littleEndian), false),
                           field.unit});
            ++fieldIndex;
        }
        return values;
    }

    values.reserve(fields.size());

    for (int i = 0; i < fields.size(); ++i)
    {
        const Field &field = fields.at(i);
        const QString name = field.name.isEmpty() ? QStringLiteral("field%1").arg(i + 1) : field.name;
        if (field.offset + field.byteLength > payload.size())
        {
            values.append({name, QStringLiteral("--"), field.unit});
            continue;
        }

        const QString value = field.type == ValueType::AsciiText || field.type == ValueType::Utf8Text
            ? formatTextValue(field, payload, field.offset)
            : formatFieldValue(field, readUnsigned(payload, field.offset, field.byteLength,
                                                    field.littleEndian), false);
        values.append({name, value, field.unit});
    }

    return values;
}

QStringList PayloadFormatter::validationWarnings(int payloadLength) const
{
    QStringList warnings;
    for (int i = 0; i < fields.size(); ++i)
    {
        const Field &field = fields.at(i);
        if (field.offset + field.byteLength > payloadLength)
        {
            const QString label = field.name.isEmpty()
                ? QStringLiteral("Field %1").arg(i + 1) : field.name;
            warnings.append(QStringLiteral("%1 requires bytes %2-%3, but payload has %4 bytes")
                .arg(label).arg(field.offset).arg(field.offset + field.byteLength - 1).arg(payloadLength));
        }
    }
    return warnings;
}

QString PayloadFormatter::sourceFormat() const
{
    return compiledFormat;
}

bool PayloadFormatter::repeatsSingleField() const
{
    return repeatField;
}
