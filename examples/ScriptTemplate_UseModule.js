var helpers = host.require("ScriptTemplate_Module.js");

function setup()
{
    host.log("Example identifier: " + helpers.hex(0x7E8));
    host.log("Bit 2 set: " + helpers.bitSet(0x04, 2));
}
