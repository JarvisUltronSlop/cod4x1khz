#include "pinc.h"
#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native_stats_probe.inc"
#include "native_identity_format_probe.inc"
#include "native_identity_contract_probe.inc"

/* Keep the pinned twenty-command budget: new identity cases share the
 * existing command rather than silently displacing the final UDP fixture. */
static void identity_dispatch(void) {
    if (Plugin_Cmd_Argc() == 1) identity_probe();
    else if (Plugin_Cmd_Argc() == 2 && !strcmp(Plugin_Cmd_Argv(1), "format")) identity_format_probe();
    else if (Plugin_Cmd_Argc() == 2 && !strcmp(Plugin_Cmd_Argv(1), "classify")) identity_classify_probe();
    else if (Plugin_Cmd_Argc() == 2 && !strcmp(Plugin_Cmd_Argv(1), "guid")) identity_guid_probe();
    else Plugin_PrintError("KHZ_IDENTITY_DISPATCH rejected=1 expected_subcommand=format|classify|guid\n");
}

static uint64_t frames, ticks, last_tick, discontinuities, move_commands;
static volatile LONG work_finished;
static int completions, runtime_allocation_rejected;
static int policy_probes;
static CONVAR_T* completed;
static int fixture_information_queries;
#include "native_thread_probe.inc"
#include "native_http_probe.inc"
#include "native_file_probe.inc"
#include "native_tcp_probe.inc"
#include "native_print_probe.inc"
#include "native_net_address_probe.inc"
#include "native_udp_probe.inc"
static void fixture_function(void) { Plugin_Scr_AddInt(10002); }
static void fixture_method(scr_entref_t entity) { Plugin_Scr_AddInt(entity.entnum); }
static void work(void* unused) { Sleep(10); InterlockedExchange(&work_finished, 1); }
static void completion(void* unused) { ++completions; Plugin_Cvar_SetInt(completed, completions); }
static void report(void) {
    Plugin_Printf("KHZ_PLUGIN_FIXTURE frames=%llu ticks=%llu gaps=%llu moves=%llu work=%ld completions=%d allocation_rejected=%d policy_probes=%d\n",
        frames, ticks, discontinuities, move_commands, InterlockedCompareExchange(&work_finished, 0, 0), completions, runtime_allocation_rejected, policy_probes);
}
static void policy_probe(void) {
    int slot, before, after;
    char* end;
    if (Plugin_Cmd_Argc() != 2) { Plugin_Printf("Usage: khz_fixture_policy <connected human slot>\n"); return; }
    slot = (int)strtol(Plugin_Cmd_Argv(1), &end, 10);
    if (*end || slot < 0 || slot >= Plugin_GetSlotCount() || !Plugin_GetPlayerID((unsigned)slot)) {
        Plugin_Printf("KHZ_PLUGIN_POLICY_FIXTURE requires a proven human admission\n"); return;
    }
    before = Plugin_CanPlayerUseCommand(slot, "khz_fixture_report");
    Plugin_AddCommandForClientToWhitelist(slot, "khz_fixture_report");
    after = Plugin_CanPlayerUseCommand(slot, "khz_fixture_report");
    Plugin_AddCommandForClientToWhitelist(slot, "khz_fixture_missing");
    if (!after || Plugin_CanPlayerUseCommand(slot, "khz_fixture_missing")) {
        Plugin_PrintError("KHZ_PLUGIN_POLICY_FIXTURE permission failure\n"); return;
    }
    ++policy_probes;
    Plugin_G_LogPrintf("KHZ_PLUGIN_GAME_LOG_FIXTURE slot=%d before=%d after=%d\n", slot, before, after);
    Plugin_PrintAdministrativeLog("KHZ_PLUGIN_ADMIN_LOG_FIXTURE slot=%d invoker=%d power=%d",
        slot, Plugin_Cmd_GetInvokerSlot(), Plugin_Cmd_GetInvokerPower());
    Plugin_Printf("KHZ_PLUGIN_POLICY_FIXTURE slot=%d before=%d after=%d probes=%d\n", slot, before, after, policy_probes);
}
PCL void OnInfoRequest(pluginInfo_t* information) {
    ++fixture_information_queries;
    memset(information, 0, sizeof(*information));
    information->handlerVersion.major = PLUGIN_HANDLER_VERSION_MAJOR;
    information->handlerVersion.minor = PLUGIN_HANDLER_VERSION_MINOR;
    information->pluginVersion.major = 1;
    strcpy(information->fullName, "cod4x1khz rebuilt SDK regression fixture");
    strcpy(information->shortDescription, "Actual dynamic functions/methods/commands, 1ms callbacks and asynchronous work");
}
PCL int OnInit(void) {
    completed = Plugin_Cvar_RegisterInt("khz_fixture_completions", 0, 0, 100, 0, "Fixture completions applied on the owner");
    if (!completed) return -1;
    Plugin_ScrAddFunction("khzfixturefunction", fixture_function);
    Plugin_ScrAddMethod("khzfixturemethod", fixture_method);
    Plugin_AddCommand("khz_fixture_report", report, 100);
    Plugin_AddCommand("khz_fixture_policy", policy_probe, 100);
    Plugin_AddCommand("khz_fixture_stats", stats_probe, 100);
    Plugin_AddCommand("khz_fixture_identity", identity_dispatch, 100);
    Plugin_AddCommand("khz_fixture_threads_start", thread_start, 100);
    Plugin_AddCommand("khz_fixture_threads_report", thread_report, 100);
    Plugin_AddCommand("khz_fixture_aliases", alias_probe, 100);
    Plugin_AddCommand("khz_fixture_http_forms", http_form_probe, 100);
    Plugin_AddCommand("khz_fixture_http_start", http_fixture_start, 100);
    Plugin_AddCommand("khz_fixture_http_report", http_fixture_report, 100);
    Plugin_AddCommand("khz_fixture_files_start", file_fixture_start, 100);
    Plugin_AddCommand("khz_fixture_files_report", file_fixture_report, 100);
    Plugin_AddCommand("khz_fixture_tcp_start", tcp_fixture_start, 100);
    Plugin_AddCommand("khz_fixture_tcp_report", tcp_fixture_report, 100);
    Plugin_AddCommand("khz_fixture_print", print_fixture_start, 100);
    Plugin_AddCommand("khz_fixture_print_report", print_fixture_report, 100);
    net_allow_reserved = Plugin_Cvar_RegisterBool("khz_fixture_net_allow_reserved", qfalse, 0, "Isolated SDK2 admission fixture reserved-slot grant");
    Plugin_AddCommand("khz_fixture_net", net_fixture_probe, 100);
    Plugin_AddCommand("khz_fixture_net_admission", net_fixture_admission_report, 100);
    Plugin_AddCommand("khz_fixture_net_ipban", net_fixture_ipban_probe, 100);
    Plugin_AddCommand("khz_fixture_udp", udp_fixture_command, 100);
    if (!Plugin_ExportFunction("khzfixturefunction", (void* (*)(void))fixture_function)) return -1;
    if (!Plugin_QueueJob(work, NULL, completion)) return -1;
    return 0;
}
PCL void OnFrame(void) {
    ++frames;
    if (frames == 1) { void* memory = Plugin_Malloc(16); runtime_allocation_rejected = memory == NULL; if (memory) Plugin_Free(memory); }
    if (frames == 1) { threadid_t id = 123; thread_tick_rejected = !Plugin_CreateNewThread(raw_thread, &id, NULL) && id == 0; }
    if (frames == 1) file_fixture_tick_probe();
    if (frames == 1) tcp_fixture_tick_probe();
    if (frames == 1) print_fixture_tick_probe();
}
PCL void OnTick(uint64_t tick, double milliseconds) {
    if (milliseconds != 1.0 || (last_tick && tick != last_tick + 1)) ++discontinuities;
    last_tick = tick; ++ticks;
}
PCL void OnSpawnServer(void) { last_tick = 0; }
PCL void OnClientMoveCommand(client_t* client, usercmd_t* command) { ++move_commands; }
PCL void OnScrUsercallFunction(const char* name) {
    if (!strcmp(name, "khzfixtureusercall")) Plugin_Scr_AddString("actual plugin usercall");
}
PCL void OnScrUsercallMethod(const char* name, int slot) {
    if (!strcmp(name, "khzfixtureusercall")) Plugin_Scr_AddInt(slot);
}
PCL void OnUnload(void) { Plugin_Printf("KHZ_PLUGIN_FIXTURE unloaded\n"); }
