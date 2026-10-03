/* Rebuilt source-compatible public HTTP response view. Legacy binary ABI is
 * intentionally unsupported; the host and plugin use this matching SDK. */
#ifndef COD4X1KHZ_PUBLIC_HTTP_API_H
#define COD4X1KHZ_PUBLIC_HTTP_API_H
#ifndef KHZ_PLUGIN_NATIVE_TYPES
typedef struct msg_t {
    int overflowed,readonly;
    unsigned char* data;
    unsigned char* splitdata;
    int maxsize,cursize,splitcursize,readcount,bit,lastRefEntity;
} msg_t;
#endif
typedef enum {FT_PROTO_HTTP,FT_PROTO_HTTPS,FT_PROTO_FTP} ftprotocols_t;
#define MAX_POST_VALS 32
struct httpPostValsInternal_s { char name[1024],value[1024]; };
typedef struct httpPostValsInternal_s httpPostVals_t[MAX_POST_VALS];
typedef struct ftRequest_t {
    int lock,active,transferactive,transferStartTime,socket,transfersocket,sentBytes,finallen,totalreceivedbytes,transfertotalreceivedbytes;
    msg_t* extrecvmsg;
    msg_t* extsendmsg;
    msg_t sendmsg,recvmsg,transfermsg;
    int complete,code,version;
    char status[32],url[1024],address[1024],username[256],password[256],contentType[64],cookie[1024];
    int mode,headerLength,contentLength,contentLengthArrived,currentChunkLength,currentChunkReadOffset,chunkedEncoding,startTime,stage;
    ftprotocols_t protocol;
    khz_public_netadr_t remote; /* Same public ABI in host/backend/rebuilt DLL. */
} ftRequest_t;
/* Make/Request copy inputs. Poll returns0 pending,1 complete HTTP response
 * (including4xx/5xx),-1 transport/validation/cancel error. Read response fields
 * only after a nonzero Poll or blocking Request return. Free invalidates it.
 * Blocking calls require an explicit host job/thread, never a gameplay tick. */
PAPI ftRequest_t* __cdecl Plugin_HTTP_Request(const char* url,const char* method,byte* payload,int length,const char* headers);
PAPI ftRequest_t* __cdecl Plugin_HTTP_GET(const char* url);
PAPI ftRequest_t* __cdecl Plugin_HTTP_MakeHttpRequest(const char* url,const char* method,byte* payload,int length,const char* headers);
PAPI int __cdecl Plugin_HTTP_SendReceiveData(ftRequest_t* request);
PAPI void __cdecl Plugin_HTTP_FreeObj(ftRequest_t* request);
PAPI void __cdecl Plugin_HTTP_CreateString_x_www_form_urlencoded(char* output,int capacity,const char* key,const char* value);
PAPI void __cdecl Plugin_HTTP_ParseFormDataBody(const char* body,httpPostVals_t* values);
PAPI const char* __cdecl Plugin_HTTP_GetFormDataItem(httpPostVals_t* values,const char* name);
#endif
