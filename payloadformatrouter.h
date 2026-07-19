#ifndef PAYLOADFORMATROUTER_H
#define PAYLOADFORMATROUTER_H

#include "can_structs.h"
#include "payloadformatter.h"

#include <QHash>

class PayloadFormatRouter
{
public:
    static QString key(int bus, quint32 id, bool extended)
    {
        if (bus < 0)
            return QStringLiteral("id:%1").arg(id);
        return QStringLiteral("%1:%2:%3").arg(bus).arg(id).arg(extended ? 1 : 0);
    }

    bool setFormat(int bus, quint32 id, bool extended, const QString &format,
                   QString *error = nullptr)
    {
        PayloadFormatter formatter;
        if (!formatter.compile(format, error))
            return false;
        formatters.insert(key(bus, id, extended), formatter);
        return true;
    }

    void clear() { formatters.clear(); }

    const PayloadFormatter *formatterFor(const CANFrame &frame) const
    {
        auto formatter = formatters.constFind(
            key(frame.bus, frame.frameId(), frame.hasExtendedFrameFormat()));
        if (formatter == formatters.constEnd())
            formatter = formatters.constFind(key(-1, frame.frameId(), false));
        return formatter == formatters.constEnd() ? nullptr : &formatter.value();
    }

private:
    QHash<QString, PayloadFormatter> formatters;
};

#endif // PAYLOADFORMATROUTER_H
