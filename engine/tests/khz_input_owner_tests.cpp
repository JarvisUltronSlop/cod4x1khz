#include <khz/khz_input_model.h>
#include <cstdio>

// This executable never registers Raw Input, creates a window, reads keyboard
// state or injects input. It tests the same production queue/control models.
int main() {
    const bool passed = khz_input::ModelSelfTest();
    std::printf("KHZ_INPUT_OWNER_TEST {\"passed\":%s,\"queue_capacity\":4096,\"interval_capacity\":128,"
        "\"coverage\":[\"bounds\",\"epochs_park_focus\",\"deadline_ordering\",\"index_and_ms_wrap\",\"window_reuse_recycle_requires_park_ack\"],"
        "\"global_input_injection\":false}\n", passed ? "true" : "false");
    return passed ? 0 : 1;
}
