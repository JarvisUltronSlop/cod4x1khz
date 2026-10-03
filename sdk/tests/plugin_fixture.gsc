// Run after loadPlugin khz_regression and a stock map's script bootstrap.
init()
{
    if ( khzfixturefunction() != 10002 )
        assertmsg("dynamic rebuilt plugin function failed");
    if ( usercall("khzfixtureusercall") != "actual plugin usercall" )
        assertmsg("public plugin usercall failed");
    println("KHZ_PLUGIN_SCRIPT_FIXTURE dynamic function and usercall passed");
    level thread player_fixture();
}

player_fixture()
{
    level waittill("connected", player);
    if ( player khzfixturemethod() != player getentitynumber() )
        assertmsg("dynamic rebuilt plugin method failed");
    if ( player usercall("khzfixtureusercall") != player getentitynumber() )
        assertmsg("public plugin method usercall failed");
    println("KHZ_PLUGIN_SCRIPT_FIXTURE dynamic method passed");
}
