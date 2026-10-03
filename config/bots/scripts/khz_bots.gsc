// cod4x1khz startup adapter for Bot Warfare v2.2.0, pinned at
// 7b52b462e40fa68287b9e54602e52da83b17766c. Upstream licenses are retained.
init()
{
    // Registration has no waits: every bot sees a complete adapter before
    // Bot Warfare's init starts callback or manager threads.
    scripts\bots_adapter_cod4x::init();
    scripts\bots::init();
    scripts\bots_chat::init();
    scripts\bots_menu::init();
    scripts\bots_wp_editor::init();
}
