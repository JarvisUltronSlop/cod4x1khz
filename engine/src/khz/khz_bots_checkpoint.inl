// Include at END of khz_bots.cpp, inside its COD4X1KHZ && KISAK_MP guard.
// The actual ScriptVM/allocator/world/Dvar providers are mandatory separately.
// No bot brain is replaced with a replay stub or a lower-frequency iterator.
#include "khz_bots_checkpoint.h"
#include "khz_server_checkpoint.h"
#include "khz_runtime.h"
#include "khz_host.h"
#include <type_traits>

namespace {
struct BotCheckpointAdmission {
    int state = 0;
    bool native = false;
    std::uintptr_t entity = 0;
    std::uint64_t generation = 0;
};
struct BotCheckpointDvar {
    const dvar_t* node = nullptr;
    unsigned type = 0;
};
struct BotCheckpointImage {
    KHZ_ServerCheckpointFrame frame{};
    decltype(controls) controls{};
    decltype(slotGenerations) generations{};
    decltype(files) closedFiles{};
    khz::Histogram work{}; // Restore instrumentation, not gameplay hash input.
    std::array<BotCheckpointAdmission, KHZ_MaxMatchClients> admissions{};
    std::array<BotCheckpointDvar, 5> dvars{};
    std::uint64_t now = 0, settingsDeadline = 0;
    std::uint64_t commandTicks = 0, playingTicks = 0, aimTicks = 0;
    std::uint64_t moveGoals = 0, aimGoals = 0, respawnPulses = 0;
    unsigned fileToken = 0;
    int maximumClients = 0, target = -1, tier = -1;
    bool scriptsStarted = false, captured = false;
};
BotCheckpointImage botCheckpointImage; // Bounded fixture-only BSS.
static_assert(std::is_trivially_copyable_v<BotControl>);
static_assert(std::is_trivially_copyable_v<ScriptFile>);

bool botCheckpointReject(char* reason, unsigned capacity, const char* text) {
    if (reason && capacity) std::snprintf(reason, capacity, "%s", text);
    return false;
}
std::array<const dvar_t*, 5> botCheckpointDvars() {
    return {enabled, desiredCount, difficulty, Dvar_FindVar("bots_manage_fill"), Dvar_FindVar("bots_skill")};
}
bool botCheckpointClosedFiles(char* reason, unsigned capacity) {
    for (const auto& file : files) {
        // Native FILE/engine handles, private read-ahead cursors and disk write
        // side effects have no rewind provider. Do not infer eligibility from
        // a settings flag or close them merely to pass capture.
        if (file.engineHandle || file.readHandle || file.writeHandle || file.token ||
            file.cursor || file.available || file.path[0])
            return botCheckpointReject(reason, capacity, "bot_checkpoint_native_script_file_has_no_cursor_provider");
    }
    return true;
}
bool botCheckpointControlsValid(char* reason, unsigned capacity) {
    for (unsigned i = 0; i != controls.size(); ++i) {
        const auto& control = controls[i];
        if (!control.generation || control.generation != slotGenerations[i] ||
            control.appliedTick > nowTick || control.weapon > 255 ||
            (control.aiming && (control.aimEnd <= control.aimBegin || control.aimBegin > nowTick)))
            return botCheckpointReject(reason, capacity, "bot_checkpoint_control_generation_or_deadline_invalid");
        for (float value : control.moveTo) if (!std::isfinite(value))
            return botCheckpointReject(reason, capacity, "bot_checkpoint_move_goal_nonfinite");
        for (float value : control.aimStart) if (!std::isfinite(value))
            return botCheckpointReject(reason, capacity, "bot_checkpoint_aim_start_nonfinite");
        for (float value : control.aimDelta) if (!std::isfinite(value))
            return botCheckpointReject(reason, capacity, "bot_checkpoint_aim_delta_nonfinite");
    }
    return true;
}
BotCheckpointAdmission botCheckpointAdmission(unsigned slotIndex) {
    const auto& client = svs.clients[slotIndex];
    return {int(client.header.state), KHZ_IsNativeBot(&client),
        reinterpret_cast<std::uintptr_t>(client.gentity), slotGenerations[slotIndex]};
}
bool botCheckpointBoundary(const KHZ_ServerCheckpointFrame& frame, char* reason, unsigned capacity) {
    if (!Sys_IsMainThread() || !KHZ_ServerCheckpointContext() || !KHZ_ServerCheckpointPaused() || com_inServerFrame)
        return botCheckpointReject(reason, capacity, "bot_checkpoint_requires_actual_paused_owner_callback");
    if (!khz::server_checkpoint::valid(frame) || frame.session != KHZ_CurrentServerSession() ||
        frame.serverAddress != reinterpret_cast<std::uintptr_t>(&sv) ||
        frame.clientsAddress != reinterpret_cast<std::uintptr_t>(svs.clients) ||
        frame.entitiesAddress != reinterpret_cast<std::uintptr_t>(g_entities) ||
        frame.nativeTime != svs.time || frame.logicalTick != KHZ_ServerTickNumber() ||
        nowTick != frame.logicalTick)
        return botCheckpointReject(reason, capacity, "bot_checkpoint_native_owner_identity_or_clock_changed");
    if (!svs.clients || !sv_maxclients || sv_maxclients->current.integer < 1 ||
        sv_maxclients->current.integer > KHZ_MaxMatchClients || !enabled || !desiredCount || !difficulty)
        return botCheckpointReject(reason, capacity, "bot_checkpoint_native_slots_or_settings_unavailable");
    if (!botCheckpointClosedFiles(reason, capacity) || !botCheckpointControlsValid(reason, capacity)) return false;
    if (nextFileToken > 0x03ffffffu || configuredTarget < -1 || configuredTarget > 15 ||
        (configuredTier != -1 && configuredTier != 1 && configuredTier != 3 && configuredTier != 5 && configuredTier != 7))
        return botCheckpointReject(reason, capacity, "bot_checkpoint_file_generation_or_manager_settings_invalid");
    for (int i = 0; i < sv_maxclients->current.integer; ++i) {
        const auto& client = svs.clients[i];
        if (KHZ_IsNativeBot(&client) && client.header.state >= CS_CONNECTED &&
            (client.header.state != CS_ACTIVE || !client.gentity || !client.gentity->client ||
                KHZ_BotClientGeneration(&client) != controls[i].generation))
            return botCheckpointReject(reason, capacity, "bot_checkpoint_native_bot_admission_not_active");
    }
    return true;
}
bool botCheckpointCapture(const KHZ_ServerCheckpointFrame& frame, char* reason, unsigned capacity) {
    botCheckpointImage.captured = false;
    if (!botCheckpointBoundary(frame, reason, capacity)) return false;
    auto& image = botCheckpointImage;
    image.controls = controls; image.generations = slotGenerations;
    image.closedFiles = files; image.work = controlWork;
    image.now = nowTick; image.settingsDeadline = nextSettings;
    image.commandTicks = commandTicks; image.playingTicks = playingTicks; image.aimTicks = aimTicks;
    image.moveGoals = moveGoals; image.aimGoals = aimGoals; image.respawnPulses = respawnPulses;
    image.fileToken = nextFileToken; image.target = configuredTarget; image.tier = configuredTier;
    image.scriptsStarted = scriptsStarted; image.maximumClients = sv_maxclients->current.integer;
    for (unsigned i = 0; i != image.admissions.size(); ++i)
        image.admissions[i] = i < unsigned(image.maximumClients) ? botCheckpointAdmission(i) : BotCheckpointAdmission{};
    const auto nodes = botCheckpointDvars();
    for (unsigned i = 0; i != nodes.size(); ++i) image.dvars[i] = {nodes[i], nodes[i] ? unsigned(nodes[i]->type) : 0u};
    image.frame = frame; image.captured = true; return true;
}
bool botCheckpointValidate(const KHZ_ServerCheckpointFrame& frame, char* reason, unsigned capacity) {
    if (!botCheckpointBoundary(frame, reason, capacity)) return false;
    const auto& image = botCheckpointImage;
    if (!image.captured || !khz::server_checkpoint::sameLifetime(image.frame, frame) ||
        image.maximumClients != sv_maxclients->current.integer)
        return botCheckpointReject(reason, capacity, "bot_checkpoint_saved_image_or_slot_arena_changed");
    for (unsigned i = 0; i < unsigned(image.maximumClients); ++i) {
        const auto current = botCheckpointAdmission(i), saved = image.admissions[i];
        // Respawn changes gameplay in the mandatory World/VM providers, but
        // keeps its real slot admission. Bot removal/recreation requires a
        // separate admission graph/lifecycle provider before it can rewind.
        if (current.state != saved.state || current.native != saved.native ||
            current.entity != saved.entity || current.generation != saved.generation)
            return botCheckpointReject(reason, capacity, "bot_checkpoint_bot_or_human_slot_admission_changed");
    }
    const auto nodes = botCheckpointDvars();
    for (unsigned i = 0; i != nodes.size(); ++i)
        if (nodes[i] != image.dvars[i].node || (nodes[i] && nodes[i]->type != image.dvars[i].type))
            return botCheckpointReject(reason, capacity, "bot_checkpoint_owned_dvar_registration_changed");
    // Actual current/latched/modified values are restored by mandatory Dvars
    // part0 inside Dvar_* ownership; this provider never bypasses that owner.
    return true;
}
void botCheckpointRestore(const KHZ_ServerCheckpointFrame&) {
    const auto& image = botCheckpointImage;
    controls = image.controls; slotGenerations = image.generations;
    files = image.closedFiles; controlWork = image.work;
    nowTick = image.now; nextSettings = image.settingsDeadline;
    commandTicks = image.commandTicks; playingTicks = image.playingTicks; aimTicks = image.aimTicks;
    moveGoals = image.moveGoals; aimGoals = image.aimGoals; respawnPulses = image.respawnPulses;
    nextFileToken = image.fileToken; configuredTarget = image.target; configuredTier = image.tier;
    scriptsStarted = image.scriptsStarted;
}
void botCheckpointDiscard() { botCheckpointImage.captured = false; }
}

void KHZ_BotRegisterServerCheckpoint() {
    owner();
    const KHZ_ServerCheckpointProvider provider{KHZ_ServerCheckpointDomain::Bots, 0, sizeof(botCheckpointImage),
        botCheckpointCapture, botCheckpointValidate, botCheckpointRestore, botCheckpointDiscard};
    if (!KHZ_ServerCheckpointRegister(provider)) Com_Error(ERR_FATAL, "Duplicate native bot checkpoint provider");
}
bool KHZ_BotCheckpointScriptFileAccessAllowed(const char* operation) {
    owner();
    if (!KHZ_ServerCheckpointPinned()) return true;
    // Coordinator records a latched eligibility failure without discarding the
    // only saved image. The caller returns its ordinary typed failure before
    // any native open/read/write/close/path creation/removal can happen.
    KHZ_ServerCheckpointRejectExternalEffect(operation ? operation : "bot_script_file_io_not_rewindable");
    return false;
}
