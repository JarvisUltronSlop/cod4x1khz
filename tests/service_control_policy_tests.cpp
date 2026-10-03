#include <service_control_policy.hpp>
#include <cstdio>
#include <string>

int main() {
    unsigned cases = 0, failures = 0;
    const auto check = [&](bool passed) { ++cases; if (!passed) ++failures; };
    // Actual stock connection phases: browser work remains enabled during
    // cinematic/logo shells, but must stop before an accepted connection.
    for (int phase = -1; phase <= 10; ++phase)
        check(khz::service_control::DisconnectedShell(phase) == (phase >= 0 && phase <= 2));
    // Both normal and overflow-recovery reliable writers use this predicate.
    // Metadata must arrive before loading/pure admission can finish.
    for (int phase = -1; phase <= 5; ++phase)
        check(khz::service_control::ReliableServerPhase(phase) == (phase >= 1 && phase <= 4));
    for (const auto text : {"khz_content_meta", "khz_content_error", "khz_content_meta 1 2", "khz_content_error 1 2"})
        check(khz::service_control::ContentReliable(text));
    for (const auto text : {"", "k", "khz_content", "khz_content_metadata", "khz_content_error_extra", " khz_content_meta", "khz_content_meta\t1", "KHZ_CONTENT_META"})
        check(!khz::service_control::ContentReliable(text));
    std::string maximum("khz_content_meta "); maximum.resize(1023, 'x');
    check(khz::service_control::ContentReliable(maximum));
    maximum.push_back('x');
    check(!khz::service_control::ContentReliable(maximum));
    std::printf("KHZ_SERVICE_CONTROL_TEST {\"cases\":%u,\"failures\":%u,\"scope\":\"native phase/control classification; real loading delivery and browser publication require game fixtures\"}\n", cases, failures);
    return failures ? 1 : 0;
}
