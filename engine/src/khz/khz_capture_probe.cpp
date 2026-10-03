#include "khz_capture_probe.h"
#include "khz_client_capture.h"
#include "khz_controls.h"
#include <qcommon/threads.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>

namespace {
std::uint64_t fixture = 0;
bool append(char* output, std::size_t size, std::size_t& used, const char* format, ...) {
    va_list args; va_start(args, format);
    const int count = std::vsnprintf(output + used, size - used, format, args);
    va_end(args);
    if (count < 0 || static_cast<std::size_t>(count) >= size - used) return false;
    used += static_cast<std::size_t>(count); return true;
}
}
const char* KHZ_ClientCaptureProbe(const char* action) {
    if (!Sys_IsMainThread() || !action) return "{\"result\":\"unavailable\"}";
    static char output[32768]; std::size_t used = 0;
    const char* result = "queried";
    bool read = false;
    if (!std::strncmp(action, "begin ", 6)) {
        unsigned samples = 0; int end = 0;
        if (std::sscanf(action + 6, "%u%n", &samples, &end) != 1 || action[6 + end] || !samples || samples > 60000)
            result = "invalid sample count";
        else {
            KHZ_CommandControlScope controls;
            if (!++fixture) ++fixture;
            // Zero is deliberate: console/OOB callers cannot assert that a
            // whole world, script VM, RNG and input timeline were restored.
            result = KHZ_ClientCaptureBegin({fixture, samples, 0}) ? "started" : "start rejected";
        }
    } else if (!std::strcmp(action, "end")) {
        KHZ_CommandControlScope controls;
        result = KHZ_ClientCaptureEnd() ? "ended" : "end rejected";
    } else if (!std::strcmp(action, "read")) read = true;
    else if (std::strcmp(action, "status")) result = "unknown action";
    append(output, sizeof(output), used, "{\"result\":\"%s\",\"records\":[", result);
    if (read) {
        khz::capture::Record record{};
        for (unsigned i = 0; i < 16 && KHZ_ClientCapturePop(&record); ++i) {
            append(output, sizeof(output), used,
                "%s{\"ordinal\":%llu,\"session\":%llu,\"generation\":%llu,\"client_tick\":%llu,\"authoritative_tick\":%llu,\"input_ack\":%llu,\"current_authoritative_tick\":%llu,\"current_input_ack\":%llu,\"current_snapshot_ack\":%llu,\"next_snapshot_ack\":%llu,\"command_number\":%d,\"command_time\":%d,\"accepted_snapshot\":%d,\"accepted_server_time\":%d,\"current_message\":%d,\"next_message\":%d,\"domain_mask\":%u,\"restore_conditions\":%u,\"domains\":[",
                i ? "," : "", record.ordinal, record.session, record.generation, record.clientTick, record.authoritativeTick,
                record.inputAck, record.currentAuthoritativeTick, record.currentInputAck, record.currentSnapshotAck,
                record.nextSnapshotAck, record.commandNumber, record.commandTime, record.acceptedSnapshot, record.acceptedServerTime,
                record.currentMessage, record.nextMessage, record.domainMask, record.restoreConditions);
            for (unsigned domain = 0; domain < khz::capture::DomainCount; ++domain) {
                const auto& digest = record.domains[domain];
                append(output, sizeof(output), used, "%s{\"first\":\"%016llx\",\"second\":\"%016llx\",\"bytes\":%llu,\"valid\":%s}",
                    domain ? "," : "", digest.first, digest.second, digest.bytes, digest.valid ? "true" : "false");
            }
            append(output, sizeof(output), used, "]}");
        }
    }
    append(output, sizeof(output), used, "],\"status\":%s}", KHZ_ClientCaptureStatusJSON());
    return output;
}
