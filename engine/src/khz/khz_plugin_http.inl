// Public names forward to the host-owned HTTP worker. Requests contain copied
// bytes and plugin generation, never mutable engine or DLL callback pointers.
namespace {
bool pluginHttpContext(void* caller, bool allowTick, int* plugin, std::uint64_t* generation) {
    if (Sys_IsMainThread()) {
        if (!ownerApiCaller(caller, allowTick)) return false;
        const auto* entry = registry.Get(activePlugin); if (!entry) return false;
        if (!allowTick && entry->state != plug::State::Enabled && entry->state != plug::State::Initializing) { ++apiRejected; return false; }
        *plugin = activePlugin; *generation = entry->generation;
    } else {
        if (activePlugin < 0 || !activeGeneration) { ++apiRejected; return false; }
        *plugin = activePlugin; *generation = activeGeneration;
    }
    return true;
}
void cancelPluginHttp(int plugin) {
    const auto* entry = registry.Get(plugin);
    if (!entry || modules[plugin].servicesCancelled) return;
    khz::plugin_http::Cancel(plugin, entry->generation);
    khz::plugin_fs::Cancel(plugin, entry->generation);
    khz::plugin_tcp::Cancel(plugin, entry->generation);
    khz::plugin_dns::Cancel(plugin, entry->generation);
    pluginNetworkCancel(unsigned(plugin), entry->generation);
    modules[plugin].servicesCancelled = true;
}
}
KHZ_API ftRequest_t* __cdecl Plugin_HTTP_MakeHttpRequest(const char* url, const char* method, byte* payload, int length, const char* headers) {
    int plugin; std::uint64_t generation;
    if (!pluginHttpContext(_ReturnAddress(), false, &plugin, &generation) || shuttingDown) return nullptr;
    if (!payload || length < 1) { payload = nullptr; length = 0; }
    return khz::plugin_http::Make(plugin, generation, url, method ? method : "GET", payload, length, headers ? headers : "");
}
KHZ_API int __cdecl Plugin_HTTP_SendReceiveData(ftRequest_t* request) {
    int plugin; std::uint64_t generation;
    return pluginHttpContext(_ReturnAddress(), true, &plugin, &generation) ? khz::plugin_http::Poll(plugin, generation, request) : -1;
}
KHZ_API void __cdecl Plugin_HTTP_FreeObj(ftRequest_t* request) {
    int plugin; std::uint64_t generation;
    if (pluginHttpContext(_ReturnAddress(), true, &plugin, &generation)) khz::plugin_http::Free(plugin, generation, request);
}
KHZ_API ftRequest_t* __cdecl Plugin_HTTP_Request(const char* url, const char* method, byte* payload, int length, const char* headers) {
    // The pinned synchronous API remains usable from explicitly pinned jobs
    // and plugin workers. Blocking the authoritative owner is rejected.
    if (Sys_IsMainThread()) { ++apiRejected; return nullptr; }
    int plugin; std::uint64_t generation;
    if (!pluginHttpContext(_ReturnAddress(), false, &plugin, &generation)) return nullptr;
    if (!payload || length < 1) { payload = nullptr; length = 0; }
    auto* request = khz::plugin_http::Make(plugin, generation, url, method ? method : "GET", payload, length, headers ? headers : "");
    if (!request) return nullptr;
    const auto deadline = GetTickCount64() + 6000;
    for (;;) {
        const int state = khz::plugin_http::Poll(plugin, generation, request);
        if (state > 0) return request;
        if (state < 0 || GetTickCount64() >= deadline) { khz::plugin_http::Free(plugin, generation, request); return nullptr; }
        Sleep(1);
    }
}
KHZ_API ftRequest_t* __cdecl Plugin_HTTP_GET(const char* url) { return Plugin_HTTP_Request(url, "GET", nullptr, 0, nullptr); }
KHZ_API void __cdecl Plugin_HTTP_CreateString_x_www_form_urlencoded(char* output, int capacity, const char* key, const char* value) {
    int plugin; std::uint64_t generation;
    if (!pluginHttpContext(_ReturnAddress(), false, &plugin, &generation) || !key || !value || capacity < 1) return;
    const auto keyLength = strnlen(key, 1024), valueLength = strnlen(value, khz::plugins::io::MaxFormText / 3 + 1);
    if (!khz::plugins::io::AppendForm(output, unsigned(capacity), std::string_view(key, keyLength), std::string_view(value, valueLength))) ++apiRejected;
}
KHZ_API void __cdecl Plugin_HTTP_ParseFormDataBody(const char* body, httpPostVals_t* values) {
    if (!values) return; std::memset(values, 0, sizeof(*values));
    int plugin; std::uint64_t generation;
    if (!pluginHttpContext(_ReturnAddress(), true, &plugin, &generation) || !body) return;
    khz::plugins::io::Form parsed;
    static_assert(sizeof(parsed) == sizeof(*values));
    const auto length = strnlen(body, khz::plugins::io::MaxFormText + 1);
    if (!khz::plugins::io::ParseForm(std::string_view(body, length), &parsed)) { ++apiRejected; return; }
    std::memcpy(values, &parsed, sizeof(parsed));
}
KHZ_API const char* __cdecl Plugin_HTTP_GetFormDataItem(httpPostVals_t* values, const char* search) {
    int plugin; std::uint64_t generation;
    if (!pluginHttpContext(_ReturnAddress(), true, &plugin, &generation) || !values || !search) return "";
    const auto searchLength = strnlen(search, 1024); if (searchLength == 1024) { ++apiRejected; return ""; }
    for (const auto& field : *values) {
        const auto keyLength = strnlen(field.name, sizeof(field.name)), valueLength = strnlen(field.value, sizeof(field.value));
        if (!keyLength) break;
        if (keyLength == sizeof(field.name) || valueLength == sizeof(field.value)) { ++apiRejected; return ""; }
        if (khz::plugins::io::Equal(std::string_view(field.name, keyLength), std::string_view(search, searchLength))) return field.value;
    }
    return "";
}
