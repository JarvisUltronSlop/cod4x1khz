static void http_form_probe(void) {
    char encoded[1024] = "";
    httpPostVals_t values;
    int form, repeated, main_blocking;
    Plugin_HTTP_CreateString_x_www_form_urlencoded(encoded, sizeof(encoded), "command", "HELO");
    Plugin_HTTP_CreateString_x_www_form_urlencoded(encoded, sizeof(encoded), "literal", "A+B &=%25");
    Plugin_HTTP_ParseFormDataBody(encoded, &values);
    form = !strcmp(encoded, "command=HELO&literal=A%2BB+%26%3D%2525") &&
        !strcmp(Plugin_HTTP_GetFormDataItem(&values, "COMMAND"), "HELO") &&
        !strcmp(Plugin_HTTP_GetFormDataItem(&values, "literal"), "A+B &=%25");
    repeated = !strcmp(Plugin_HTTP_GetFormDataItem(&values, "literal"), "A+B &=%25") &&
        !*Plugin_HTTP_GetFormDataItem(&values, "missing");
    Plugin_HTTP_ParseFormDataBody("bad=%00", &values);
    form &= !*Plugin_HTTP_GetFormDataItem(&values, "bad");
    main_blocking = Plugin_HTTP_GET("http://127.0.0.1:1/never") == NULL;
    Plugin_Printf("KHZ_PLUGIN_HTTP_FORM_FIXTURE form=%d repeated=%d main_blocking_rejected=%d\n", form, repeated, main_blocking);
}
static char http_fixture_url[1024];
static volatile LONG http_fixture_active, http_fixture_work_valid;
static int http_fixture_owner_valid, http_fixture_completions;
static void http_fixture_work(void* argument) {
    char payload[] = "command=HELO&literal=A%2BB+%26%3D%2525";
    httpPostVals_t values;
    ftRequest_t* response = Plugin_HTTP_Request(http_fixture_url, "POST", (byte*)payload, (int)strlen(payload),
        "Content-Type: application/x-www-form-urlencoded\r\nAuthorization: fixture-token\r\n");
    int valid = 0;
    if (response && response->code == 200 && response->complete && response->extrecvmsg && response->headerLength > 0 &&
        response->contentLength > 0 && response->contentLength <= 4096) {
        char body[4097];
        memcpy(body, response->extrecvmsg->data + response->headerLength, response->contentLength); body[response->contentLength] = 0;
        Plugin_HTTP_ParseFormDataBody(body, &values);
        valid = !strcmp(Plugin_HTTP_GetFormDataItem(&values, "status"), "okay") &&
            !strcmp(Plugin_HTTP_GetFormDataItem(&values, "literal"), "A+B &=%25");
    }
    if (response) Plugin_HTTP_FreeObj(response);
    InterlockedExchange(&http_fixture_work_valid, valid);
}
static void http_fixture_completion(void* argument) {
    http_fixture_owner_valid = Plugin_IsMainThread() && !Plugin_IsAuthoritativeTick(); ++http_fixture_completions;
    InterlockedExchange(&http_fixture_active, 0);
}
static void http_fixture_start(void) {
    if (Plugin_Cmd_Argc() != 2 || strlen(Plugin_Cmd_Argv(1)) >= sizeof(http_fixture_url)) { Plugin_Printf("Usage: khz_fixture_http_start <controlled HTTP fixture URL>\n"); return; }
    if (InterlockedCompareExchange(&http_fixture_active, 1, 0)) { Plugin_Printf("KHZ_PLUGIN_HTTP_FIXTURE busy\n"); return; }
    strcpy(http_fixture_url, Plugin_Cmd_Argv(1)); InterlockedExchange(&http_fixture_work_valid, 0);
    if (!Plugin_QueueJob(http_fixture_work, NULL, http_fixture_completion)) { InterlockedExchange(&http_fixture_active, 0); Plugin_PrintError("KHZ_PLUGIN_HTTP_FIXTURE enqueue failed\n"); }
}
static void http_fixture_report(void) {
    Plugin_Printf("KHZ_PLUGIN_HTTP_FIXTURE active=%ld worker_response=%ld completions=%d owner=%d\n",
        InterlockedCompareExchange(&http_fixture_active,0,0), InterlockedCompareExchange(&http_fixture_work_valid,0,0), http_fixture_completions, http_fixture_owner_valid);
}
