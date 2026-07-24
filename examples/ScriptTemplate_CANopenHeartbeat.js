var busNumber = 0;

function setup()
{
    can.setFilter(0x700, 0x780, busNumber);
    host.log("Monitoring CANopen heartbeat and boot-up traffic");
}

function gotCANFrame(bus, id, len, data)
{
    var nodeId = id - 0x700;
    var state = format.decode(
        "state:u8{0:Boot-up,4:Stopped,5:Operational,127:Pre-operational,*:Unknown}",
        data);
    host.log("Node " + nodeId + " " + state);
}
