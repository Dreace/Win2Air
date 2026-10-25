#include "target_probe.h"
#include "platform.h"
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <plist/plist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

struct probe_context {
    bool done, headers;
    size_t length;
    sender_result result;
    sender_probe_info value;
    sender_error error;
};
static void fail(struct probe_context* ctx,sender_result result,const char* message,int peer) {
    ctx->done=true;ctx->result=result;ctx->error.code=result;ctx->error.peer_code=peer;
    snprintf(ctx->error.operation,sizeof(ctx->error.operation),"probe-info");
    snprintf(ctx->error.message,sizeof(ctx->error.message),"%s",message);
}
static bool text(plist_t root,const char* key,char* output,size_t capacity) {
    plist_t node=plist_dict_get_item(root,key);char* value=NULL;
    if(!node || plist_get_node_type(node)!=PLIST_STRING) return false;
    plist_get_string_val(node,&value);
    bool valid=value && value[0] && strlen(value)<capacity;
    if(valid) memcpy(output,value,strlen(value)+1);
    free(value);return valid;
}
static bool number(plist_t root,const char* key,uint64_t* value) {
    plist_t node=plist_dict_get_item(root,key);
    if(!node) return false;
    if(plist_get_node_type(node)==PLIST_UINT) { plist_get_uint_val(node,value);return true; }
    if(plist_get_node_type(node)==PLIST_STRING) {
        char* str=NULL;plist_get_string_val(node,&str);
        char* end=NULL;errno=0;
        unsigned long long parsed=str ? strtoull(str,&end,0) : 0;
        bool valid=str && str[0] && str[0]!='-' && !errno && end && !*end;
        free(str);if(valid) *value=parsed;return valid;
    }
    return false;
}
static void body(struct probe_context* ctx,const char* bytes,uint32_t length) {
    plist_t root=NULL;
    if(length>=8 && !memcmp(bytes,"bplist00",8)) plist_from_bin(bytes,length,&root);
    else plist_from_xml(bytes,length,&root);
    if(!root || plist_get_node_type(root)!=PLIST_DICT) { if(root) plist_free(root);fail(ctx,SENDER_INPUT,"Receiver info is not a valid property-list dictionary",0);return; }
    bool identity=text(root,"deviceID",ctx->value.receiver_id,sizeof(ctx->value.receiver_id));
    if(identity) {
        identity=strlen(ctx->value.receiver_id)==17;
        for(unsigned i=0;i<17 && identity;++i) {
            if(i%3==2) identity=ctx->value.receiver_id[i]==':';
            else { identity=isxdigit((unsigned char)ctx->value.receiver_id[i])!=0;ctx->value.receiver_id[i]=(char)toupper((unsigned char)ctx->value.receiver_id[i]); }
        }
    }
    if(!identity || !text(root,"name",ctx->value.name,sizeof(ctx->value.name)) || !text(root,"model",ctx->value.model,sizeof(ctx->value.model)))
        fail(ctx,SENDER_INPUT,"Receiver info lacks a valid public identity, name or model",0);
    else {
        if (plist_dict_get_item(root, "firmwareVersion") &&
            !text(root, "firmwareVersion", ctx->value.firmware, sizeof(ctx->value.firmware))) {
            fail(ctx, SENDER_INPUT, "Receiver firmware metadata is invalid", 0);
            plist_free(root);
            return;
        }
        ctx->value.features_available=number(root,"features",&ctx->value.features);
        number(root,"statusFlags",&ctx->value.status_flags);
        ctx->result=SENDER_OK;ctx->done=true;
    }
    plist_free(root);
}
static void read_info(struct bufferevent* bev,void* context) {
    struct probe_context* ctx=context;struct evbuffer* input=bufferevent_get_input(bev);
    if(!ctx->headers) {
        struct evbuffer_ptr end=evbuffer_search(input,"\r\n\r\n",4,NULL);
        if(end.pos<0) { if(evbuffer_get_length(input)>8192) fail(ctx,SENDER_INPUT,"Receiver response headers exceed the bound",0);return; }
        if(end.pos>8192) { fail(ctx,SENDER_INPUT,"Receiver response headers exceed the bound",0);return; }
        char headers[8197];size_t count=(size_t)end.pos+4;evbuffer_remove(input,headers,count);headers[count]=0;
        int status=0;char* first=strstr(headers,"\r\n");
        if(!first || sscanf(headers,"RTSP/1.0 %d",&status)!=1) { fail(ctx,SENDER_INPUT,"Receiver response has an invalid RTSP status",0);return; }
        if(status!=200) { fail(ctx,status==401 || status==403 || status==470 ? SENDER_AUTHENTICATION : SENDER_NETWORK,"Receiver rejected the public info request",status);return; }
        bool found=false;char* line=first+2;
        while(*line && strncmp(line,"\r\n",2)) {
            char* next=strstr(line,"\r\n");if(!next) { fail(ctx,SENDER_INPUT,"Receiver response contains an incomplete header",0);return; }*next=0;
            if(!strncasecmp(line,"Content-Length:",15)) {
                char* value=line+15;while(*value==' ' || *value=='\t') ++value;char* tail=NULL;errno=0;
                unsigned long length=strtoul(value,&tail,10);
                if(found || !isdigit((unsigned char)*value) || errno || !tail || *tail || !length || length>65536) { fail(ctx,SENDER_INPUT,"Receiver info length is duplicated, malformed or exceeds the bound",0);return; }
                ctx->length=length;found=true;
            }
            line=next+2;
        }
        if(!found) { fail(ctx,SENDER_INPUT,"Receiver info response has no bounded content length",0);return; }ctx->headers=true;
    }
    if(evbuffer_get_length(input)>=ctx->length) {
        char* bytes=malloc(ctx->length);if(!bytes) { fail(ctx,SENDER_PLATFORM,"Allocate receiver info failed",0);return; }
        evbuffer_remove(input,bytes,ctx->length);body(ctx,bytes,(uint32_t)ctx->length);free(bytes);
    }
}
static void event_info(struct bufferevent* bev,short events,void* context) {
    struct probe_context* ctx=context;
    if(events&BEV_EVENT_CONNECTED) {
        const char request[]="GET /info RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: AirPlay/999.0.0\r\nX-Apple-Client-Name: Win2Air\r\nContent-Length: 0\r\n\r\n";
        if(bufferevent_write(bev,request,sizeof(request)-1)) fail(ctx,SENDER_PLATFORM,"Queue public info request failed",0);
    } else if(!ctx->done && (events&(BEV_EVENT_EOF|BEV_EVENT_ERROR|BEV_EVENT_TIMEOUT))) {
        fail(ctx,SENDER_NETWORK,"Receiver closed or failed before returning complete public info",0);ctx->error.platform_code=EVUTIL_SOCKET_ERROR();
    }
}
static uint64_t now_ms(void) { struct timespec now;if(platform_time(PLATFORM_MONOTONIC,&now)<0) return 0;return (uint64_t)now.tv_sec*1000+now.tv_nsec/1000000; }
sender_result target_probe_run(const sender_probe_request* request,sender_probe_info* output,sender_error* error) {
    struct probe_context ctx={0};ctx.value.size=sizeof(ctx.value);ctx.value.version=1;ctx.value.probe_id=request->probe_id;
    struct sockaddr_storage address={0};int length;
    struct sockaddr_in* v4=(void*)&address;struct sockaddr_in6* v6=(void*)&address;
    if(evutil_inet_pton(AF_INET,request->address,&v4->sin_addr)==1 && !request->scope_id) { v4->sin_family=AF_INET;v4->sin_port=htons(request->port);length=sizeof(*v4); }
    else if(evutil_inet_pton(AF_INET6,request->address,&v6->sin6_addr)==1) { v6->sin6_family=AF_INET6;v6->sin6_port=htons(request->port);v6->sin6_scope_id=request->scope_id;length=sizeof(*v6); }
    else { fail(&ctx,SENDER_INPUT,"Public info probe requires a resolved numeric address and valid scope",0);if(error) *error=ctx.error;return ctx.result; }
    if((address.ss_family==AF_INET && (v4->sin_addr.s_addr==INADDR_ANY || v4->sin_addr.s_addr==INADDR_BROADCAST || IN_MULTICAST(ntohl(v4->sin_addr.s_addr)))) ||
       (address.ss_family==AF_INET6 && (IN6_IS_ADDR_UNSPECIFIED(&v6->sin6_addr) || IN6_IS_ADDR_MULTICAST(&v6->sin6_addr) || (IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr) && !request->scope_id)))) {
        fail(&ctx,SENDER_INPUT,"Public info target must be unicast; link-local IPv6 requires an interface scope",0);if(error) *error=ctx.error;return ctx.result;
    }
    struct event_base* base=event_base_new();struct bufferevent* bev=base ? bufferevent_socket_new(base,-1,BEV_OPT_CLOSE_ON_FREE) : NULL;
    if(!bev) fail(&ctx,SENDER_PLATFORM,"Create bounded public info connection failed",0);
    else {
        bufferevent_setcb(bev,read_info,NULL,event_info,&ctx);bufferevent_setwatermark(bev,EV_READ,0,73732);bufferevent_enable(bev,EV_READ|EV_WRITE);
        if(bufferevent_socket_connect(bev,(void*)&address,length)<0) fail(&ctx,SENDER_NETWORK,"Connect public info target failed",0);
        uint64_t start=now_ms();
        while(!ctx.done) {
            if(request->should_cancel && request->should_cancel(request->context)) { fail(&ctx,SENDER_CANCELED,"Public info probe was canceled",0);break; }
            uint64_t now=now_ms();if(!start || !now || now-start>=request->timeout_ms) { fail(&ctx,SENDER_TIMEOUT,"Public info probe exceeded its total deadline",0);break; }
            struct timeval wait={0,20000};event_base_loopexit(base,&wait);event_base_dispatch(base);
        }
    }
    if(bev) bufferevent_free(bev);
    if(base) event_base_free(base);
    if(ctx.result==SENDER_OK) *output=ctx.value;
    if(error) *error=ctx.error;
    return ctx.result;
}
