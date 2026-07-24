var busNumber = 0;
var requestId = 0x7E0;
var responseId = 0x7E8;
var did = 0xF190;

function setup()
{
    uds.setFilter(responseId, 0x7FF, busNumber);
    uds.sendUDS(busNumber, requestId, 0x22, 2, did, 0, []);
    host.log("Requested DID 0x" + did.toString(16).toUpperCase());
}

function gotUDSMessage(bus, id, service, subfunc, len, data)
{
    host.log("UDS response: " + format.decode("vin:ascii17", data));
}
