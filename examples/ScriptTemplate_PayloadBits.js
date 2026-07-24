function setup()
{
    var data = [0x86, 0x1C, 0x84];
    host.log(format.decode(
        "enabled:bool1@0 warning:flag7@0 rpm:u16be@1/4[rpm]{0}", data));

    var decoded = format.fields("ready:bit2@0 state:bit1@0{0:Idle,1:Active}", data);
    for (var i = 0; i < decoded.length; ++i)
        host.log(decoded[i].name + " = " + decoded[i].value);
}
