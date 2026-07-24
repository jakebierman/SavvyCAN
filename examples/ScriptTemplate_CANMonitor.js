var busNumber = 0;
var frameId = 0x123;

function setup()
{
    can.setFilter(frameId, 0x7FF, busNumber);
    host.log("Monitoring CAN ID 0x" + frameId.toString(16).toUpperCase());
}

function gotCANFrame(bus, id, len, data)
{
    host.log(format.decode("flags:flag0@0 value:u16be@1", data));
}
