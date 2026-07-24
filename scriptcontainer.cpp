#include <QJSValueIterator>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "scriptcontainer.h"
#include "connections/canconmanager.h"

ScriptContainer::ScriptContainer()
{
    qDebug() << "Script Container Constructor";
    scriptEngine = new QJSEngine();
    canHelper = new CANScriptHelper(scriptEngine);
    isoHelper = new ISOTPScriptHelper(scriptEngine);
    udsHelper = new UDSScriptHelper(scriptEngine);
    formatHelper = new PayloadFormatScriptHelper(scriptEngine);
    connect(&timer, SIGNAL(timeout()), this, SLOT(tick()));
}

ScriptContainer::~ScriptContainer()
{
    qDebug() << "Script Container Destructor " << (uint64_t)this << "c: " << (uint64_t)canHelper;
    timer.stop();
    disconnect(&timer, SIGNAL(timeout()), this, SLOT(tick()));
    if (scriptEngine)
    {
        scriptText = "";
        compileScript();
        //delete scriptEngine;   //doing this here seems to cause a crash. No crash if you don't.
        //scriptEngine = nullptr;
    }
    if (canHelper)
    {
        delete canHelper;
        canHelper = nullptr;
    }
    if (isoHelper)
    {
        delete isoHelper;
        isoHelper = nullptr;
    }
    if (udsHelper)
    {
        delete udsHelper;
        udsHelper = nullptr;
    }
    delete formatHelper;
    formatHelper = nullptr;
    qDebug() << "end of destruct";
}

void ScriptContainer::compileScript()
{
    emit sendLog("Compiling script...");

    canHelper->clearFilters();
    isoHelper->clearFilters();
    udsHelper->clearFilters();
    loadedModules.clear();

    // Install helpers before evaluation so modules and top-level setup code can use them.
    scriptEngine->globalObject().setProperty("host", scriptEngine->newQObject(this));
    scriptEngine->globalObject().setProperty("can", scriptEngine->newQObject(canHelper));
    scriptEngine->globalObject().setProperty("isotp", scriptEngine->newQObject(isoHelper));
    scriptEngine->globalObject().setProperty("uds", scriptEngine->newQObject(udsHelper));
    scriptEngine->globalObject().setProperty("format", scriptEngine->newQObject(formatHelper));

    QJSValue result = scriptEngine->evaluate(scriptText, filePath.isEmpty() ? fileName : filePath);

    if (result.isError())
    {

        emit sendLog("SCRIPT EXCEPTION!");
        emit sendLog("Line: " + result.property("lineNumber").toString());
        emit sendLog(result.property("message").toString());
        emit sendLog("Stack:");
        emit sendLog(result.property("stack").toString());
    }
    else
    {
        compiledScript = result;

        //Find out which callbacks the script has created.
        setupFunction = scriptEngine->globalObject().property("setup");
        canHelper->setRxCallback(scriptEngine->globalObject().property("gotCANFrame"));
        isoHelper->setRxCallback(scriptEngine->globalObject().property("gotISOTPMessage"));
        udsHelper->setRxCallback(scriptEngine->globalObject().property("gotUDSMessage"));

        tickFunction = scriptEngine->globalObject().property("tick");

        if (setupFunction.isCallable())
        {
            qDebug() << "setup exists";
            QJSValue res = setupFunction.call();
            if (res.isError())
            {
                emit sendLog("Error in setup function on line " + res.property("lineNumber").toString());
                emit sendLog(res.property("message").toString());
            }
        }
        if (tickFunction.isCallable()) qDebug() << "tick exists";
    }
}

QJSValue ScriptContainer::require(QJSValue requestedName)
{
    QString requested = requestedName.toString().trimmed();
    if (requested.isEmpty())
    {
        scriptEngine->throwError(QStringLiteral("host.require() needs a module filename"));
        return QJSValue();
    }
    if (!requested.endsWith(QStringLiteral(".js"), Qt::CaseInsensitive))
        requested += QStringLiteral(".js");

    const QDir baseDir = filePath.isEmpty() ? QDir::current() : QFileInfo(filePath).absoluteDir();
    const QString resolved = QFileInfo(baseDir.filePath(requested)).canonicalFilePath();
    if (resolved.isEmpty())
    {
        scriptEngine->throwError(QStringLiteral("Module not found: %1").arg(requested));
        return QJSValue();
    }

    const auto cached = loadedModules.constFind(resolved);
    if (cached != loadedModules.constEnd())
        return cached.value();

    QFile moduleFile(resolved);
    if (!moduleFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        scriptEngine->throwError(QStringLiteral("Cannot read module: %1").arg(resolved));
        return QJSValue();
    }

    const QString source = QString::fromUtf8(moduleFile.readAll());
    const QString wrapper = QStringLiteral(
        "(function(exports, module, __filename, __dirname) {\n%1\n;return module.exports;\n})")
        .arg(source);
    QJSValue factory = scriptEngine->evaluate(wrapper, resolved);
    if (factory.isError())
        return factory;

    QJSValue exports = scriptEngine->newObject();
    QJSValue module = scriptEngine->newObject();
    module.setProperty(QStringLiteral("exports"), exports);
    QJSValueList args;
    args << exports << module << resolved << QFileInfo(resolved).absolutePath();
    QJSValue result = factory.call(args);
    if (!result.isError())
        loadedModules.insert(resolved, result);
    return result;
}

PayloadFormatScriptHelper::PayloadFormatScriptHelper(QJSEngine *engine) :
    scriptEngine(engine)
{
}

QByteArray PayloadFormatScriptHelper::byteArray(const QJSValue &data, QString *error) const
{
    if (!data.isArray())
    {
        *error = QStringLiteral("Payload must be a JavaScript byte array");
        return QByteArray();
    }

    const int length = data.property(QStringLiteral("length")).toInt();
    QByteArray payload(length, 0);
    for (int i = 0; i < length; ++i)
    {
        const int value = data.property(static_cast<quint32>(i)).toInt();
        if (value < 0 || value > 255)
        {
            *error = QStringLiteral("Payload byte %1 is outside 0..255").arg(i);
            return QByteArray();
        }
        payload[i] = static_cast<char>(value);
    }
    error->clear();
    return payload;
}

QJSValue PayloadFormatScriptHelper::errorValue(const QString &message) const
{
    scriptEngine->throwError(message);
    return QJSValue();
}

QJSValue PayloadFormatScriptHelper::decode(QJSValue expression, QJSValue data)
{
    PayloadFormatter formatter;
    QString error;
    if (!formatter.compile(expression.toString(), &error))
        return errorValue(error);
    const QByteArray payload = byteArray(data, &error);
    if (!error.isEmpty())
        return errorValue(error);
    return QJSValue(formatter.format(payload));
}

QJSValue PayloadFormatScriptHelper::fields(QJSValue expression, QJSValue data)
{
    PayloadFormatter formatter;
    QString error;
    if (!formatter.compile(expression.toString(), &error))
        return errorValue(error);
    const QByteArray payload = byteArray(data, &error);
    if (!error.isEmpty())
        return errorValue(error);

    const QVector<PayloadFormatter::FormattedField> decoded = formatter.formatFields(payload);
    QJSValue result = scriptEngine->newArray(static_cast<uint>(decoded.size()));
    for (int i = 0; i < decoded.size(); ++i)
    {
        QJSValue field = scriptEngine->newObject();
        field.setProperty(QStringLiteral("name"), decoded.at(i).name);
        field.setProperty(QStringLiteral("value"), decoded.at(i).value);
        field.setProperty(QStringLiteral("unit"), decoded.at(i).unit);
        result.setProperty(static_cast<quint32>(i), field);
    }
    return result;
}

void ScriptContainer::setScriptWindow(ScriptingWindow *win)
{
    window = win;
    connect(this, &ScriptContainer::sendLog, window, &ScriptingWindow::log);
}

void ScriptContainer::log(QJSValue logString)
{
    QString val = logString.toString();
    emit sendLog(val);
}

void ScriptContainer::setTickInterval(QJSValue interval)
{
    int intervalValue = interval.toInt();
    qDebug() << "called set tick interval with value " << intervalValue;
    if (intervalValue > 0)
    {
        timer.setInterval(intervalValue);
        timer.start();
    }
    else timer.stop();
}

void ScriptContainer::tick()
{
    if (tickFunction.isCallable())
    {
        //qDebug() << "Calling tick function";
        QJSValue res = tickFunction.call();
        if (res.isError())
        {
            emit sendLog("Error in tick function on line " + res.property("lineNumber").toString());
            emit sendLog(res.property("message").toString());
        }
    }
}

void ScriptContainer::addParameter(QJSValue name)
{
    scriptParams.append(name.toString());
}

void ScriptContainer::updateValuesTable(QTableWidget *widget)
{
    QString value;

    foreach (QString paramName, scriptParams)
    {
        value = scriptEngine->globalObject().property(paramName).toString();
        qDebug() << paramName << " - " << value;
        bool found = false;
        for (int i = 0; i < widget->rowCount(); i++)
        {
            if (widget->item(i, 0) && widget->item(i, 0)->text().compare(paramName) == 0)
            {
                found = true;
                if (!widget->item(i, 1)->isSelected())
                {
                    widget->item(i,1)->setText(value);
                }
                break;
            }
        }
        if (!found)
        {
            int row = widget->rowCount();
            widget->insertRow(widget->rowCount());
            QTableWidgetItem *item;
            item = new QTableWidgetItem();
            item->setText(paramName);
            item->setFlags(Qt::ItemIsEnabled);
            widget->setItem(row, 0, item);
            item = new QTableWidgetItem();
            item->setText(value);
            widget->setItem(row, 1, item);
        }
    }
}

void ScriptContainer::updateParameter(QString name, QString value)
{
    qDebug() << name << " * " << value;
    QJSValue val(value);
    scriptEngine->globalObject().setProperty(name, val);
}




/* CANScriptHandler Methods */

CANScriptHelper::CANScriptHelper(QJSEngine *engine)
{
    scriptEngine = engine;
}

void CANScriptHelper::setRxCallback(QJSValue cb)
{
    gotFrameFunction = cb;
}

void CANScriptHelper::setFilter(QJSValue id, QJSValue mask, QJSValue bus)
{
    uint32_t idVal = id.toUInt();
    uint32_t maskVal = mask.toUInt();
    int busVal = bus.toInt();
    qDebug() << "Called set filter";
    qDebug() << idVal << "*" << maskVal << "*" << busVal;
    CANFilter filter;
    filter.setFilter(idVal, maskVal, busVal);
    filters.append(filter);

    CANConManager::getInstance()->addTargettedFrame(busVal, idVal, maskVal, this);
}

void CANScriptHelper::clearFilters()
{
    qDebug() << "Called clear filters";
    foreach (CANFilter filter, filters)
    {
        CANConManager::getInstance()->removeTargettedFrame(filter.bus, filter.ID, filter.mask, this);
    }

    filters.clear();
}

void CANScriptHelper::sendFrame(QJSValue bus, QJSValue id, QJSValue length, QJSValue data)
{
    CANFrame frame;
    frame.setExtendedFrameFormat(false);
    frame.setFrameId(static_cast<uint32_t>(id.toInt()));
    QByteArray bytes(length.toUInt(), 0);

    if (!data.isArray()) qDebug() << "data isn't an array";

    for (int i = 0; i < bytes.length(); i++)
    {
        bytes[i] = (uint8_t)data.property(i).toInt();
    }
    frame.setPayload(bytes);
    frame.bus = (uint32_t)bus.toInt();
    //if (frame.bus > 1) frame.bus = 1;

    if (frame.frameId() > 0x7FF) frame.setExtendedFrameFormat(true);

    qDebug() << "sending frame from script";
    CANConManager::getInstance()->sendFrame(frame);
}

void CANScriptHelper::gotTargettedFrame(const CANFrame &frame)
{
    if (!gotFrameFunction.isCallable()) return; //nothing to do if we can't even call the function
    //qDebug() << "Got frame in script interface";

    const unsigned char *data = reinterpret_cast<const unsigned char *>(frame.payload().constData());
    int dataLen = frame.payload().length();

    for (int i = 0; i < filters.length(); i++)
    {
        if (filters[i].checkFilter(frame.frameId(), frame.bus))
        {
            QJSValueList args;
            args << frame.bus << frame.frameId() << static_cast<uint>(frame.payload().length());
            QJSValue dataBytes = scriptEngine->newArray(dataLen);

            for (int j = 0; j < dataLen; j++) dataBytes.setProperty(j, QJSValue(data[j]));
            args.append(dataBytes);
            gotFrameFunction.call(args);
            return; //as soon as one filter matches we jump out
        }
    }
}




/* ISOTPScriptHelper methods */
ISOTPScriptHelper::ISOTPScriptHelper(QJSEngine *engine)
{
    scriptEngine = engine;
    handler = new ISOTP_HANDLER;
    connect(handler, SIGNAL(newISOMessage(ISOTP_MESSAGE)), this, SLOT(newISOMessage(ISOTP_MESSAGE)));
    handler->setReception(true);
    handler->setFlowCtrl(true);
}

void ISOTPScriptHelper::clearFilters()
{
    handler->clearAllFilters();
}

void ISOTPScriptHelper::setFilter(QJSValue id, QJSValue mask, QJSValue bus)
{
    uint32_t idVal = id.toUInt();
    uint32_t maskVal = mask.toUInt();
    int busVal = bus.toInt();
    qDebug() << "Called ISOTP set filter";
    qDebug() << idVal << "*" << maskVal << "*" << busVal;

    handler->addFilter(busVal, idVal, maskVal);
}

void ISOTPScriptHelper::sendISOTP(QJSValue bus, QJSValue id, QJSValue length, QJSValue dataBytes)
{
    ISOTP_MESSAGE msg;
    msg.setExtendedFrameFormat(false);
    msg.setFrameId(id.toUInt());
    QByteArray dataArray(length.toUInt(), 0);

    int dataLen = dataArray.length();

    if (!dataBytes.isArray()) qDebug() << "data isn't an array";

    for (int i = 0; i < dataLen; i++)
    {
        dataArray[i] = static_cast<uint8_t>(dataBytes.property(i).toInt());
    }
    msg.setPayload(dataArray);

    msg.bus = bus.toInt();

    if (msg.frameId() > 0x7FF) msg.setExtendedFrameFormat(true);

    qDebug() << "sending ISOTP message from script";
    
    handler->sendISOTPFrame(msg.bus, msg.frameId(), msg.payload());
}

void ISOTPScriptHelper::setRxCallback(QJSValue cb)
{
    gotFrameFunction = cb;
}

void ISOTPScriptHelper::newISOMessage(ISOTP_MESSAGE msg)
{
    qDebug() << "isotpScriptHelper got a ISOTP message";
    if (!gotFrameFunction.isCallable()) return; //nothing to do if we can't even call the function
    //qDebug() << "Got frame in script interface";

    QJSValueList args;
    args << msg.bus << msg.frameId() << static_cast<uint>(msg.payload().length());
    QJSValue dataBytes = scriptEngine->newArray(static_cast<uint>(msg.payload().length()));

    for (int j = 0; j < msg.payload().length(); j++) dataBytes.setProperty(static_cast<quint32>(j), QJSValue((unsigned char)msg.payload()[j]));
    args.append(dataBytes);
    gotFrameFunction.call(args);
}




/* UDSScriptHelper methods */
UDSScriptHelper::UDSScriptHelper(QJSEngine *engine)
{
    scriptEngine = engine;
    handler = new UDS_HANDLER;
    connect(handler, SIGNAL(newUDSMessage(UDS_MESSAGE)), this, SLOT(newUDSMessage(UDS_MESSAGE)));
    handler->setReception(true);
    handler->setFlowCtrl(true); //uds potentially requires flow control so turn it on
}

void UDSScriptHelper::clearFilters()
{
    handler->clearAllFilters();
}

void UDSScriptHelper::setFilter(QJSValue id, QJSValue mask, QJSValue bus)
{
    uint32_t idVal = id.toUInt();
    uint32_t maskVal = mask.toUInt();
    int busVal = bus.toInt();
    qDebug() << "Called UDS set filter";
    qDebug() << idVal << "*" << maskVal << "*" << busVal;

    handler->addFilter(busVal, idVal, maskVal);
}

void UDSScriptHelper::sendUDS(QJSValue bus, QJSValue id, QJSValue service, QJSValue sublen, QJSValue subFunc, QJSValue length, QJSValue dataBytes)
{
    UDS_MESSAGE msg;
    msg.setExtendedFrameFormat(false);
    msg.setFrameId(id.toUInt());
    QByteArray dataArray(length.toUInt(), 0);
    msg.service = service.toUInt();
    msg.subFuncLen = sublen.toUInt();
    msg.subFunc = subFunc.toUInt();

    int dataLen = dataArray.length();

    if (!dataBytes.isArray()) qDebug() << "data isn't an array";

    for (int i = 0; i < dataLen; i++)
    {
        dataArray[i] = static_cast<uint8_t>(dataBytes.property(i).toInt());
    }
    msg.setPayload(dataArray);

    msg.bus = bus.toInt();

    if (msg.frameId() > 0x7FF) msg.setExtendedFrameFormat( true );

    qDebug() << "sending UDS message from script";

    handler->sendUDSFrame(msg);
}

void UDSScriptHelper::setRxCallback(QJSValue cb)
{
    gotFrameFunction = cb;
}

void UDSScriptHelper::newUDSMessage(UDS_MESSAGE msg)
{
    //qDebug() << "udsScriptHelper got a UDS message";
    qDebug() << "UDS script helper. Msg data len: " << msg.payload().length();
    if (!gotFrameFunction.isCallable()) return; //nothing to do if we can't even call the function
    qDebug() << "Got frame in script interface";

    QJSValueList args;
    args << msg.bus << msg.frameId() << msg.service << msg.subFunc << static_cast<uint>(msg.payload().length());
    QJSValue dataBytes = scriptEngine->newArray(static_cast<unsigned int>(msg.payload().length()));

    for (int j = 0; j < msg.payload().length(); j++) dataBytes.setProperty(static_cast<quint32>(j), QJSValue((unsigned char)msg.payload()[j]));
    args.append(dataBytes);
    gotFrameFunction.call(args);
}
