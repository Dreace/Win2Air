#define _GNU_SOURCE
#include "trace.h"
#include "srp_group.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <gcrypt.h>
#include <plist/plist.h>
#include "pair-tlv.h"
#include "codec.c"
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sodium.h>
struct audio_sample { uint16_t sequence; uint32_t timestamp; uint8_t flags[2],ssrc[4],digest[32]; size_t length; };
#define AUDIO_PACKET_LIMIT 20000
enum direct_purpose { DIRECT_MULTIPLIER, DIRECT_SCRAMBLE, DIRECT_PASSWORD, DIRECT_SESSION, DIRECT_MODULUS, DIRECT_GENERATOR, DIRECT_IDENTITY, DIRECT_COUNT };
struct capture { struct protocol_trace control,info; uint8_t key[32],direct[DIRECT_COUNT][64],salt[16],public_a[384],public_b[384]; size_t direct_count,salt_len,a_len,b_len; int client_proof,server_proof,have_key,maintenance_response; unsigned pairs,authenticated,packets,discovery,feedback,maintenance_requests,sync_count; time_t first_date,last_date; uint64_t record_time,record_sequence,clock_id,last_sync_ns; uint32_t last_sync_pos; int have_record,pcm,have_cseq,pending; unsigned long request_cseq; struct audio_sample *samples; AVCodecContext *decoder; struct sender_codec *encoder; };
#define REQUIRE(c) do { if(!(c)) { fprintf(stderr,"operation=capture-parse check=%s line=%d\n",#c,__LINE__); return -1; } } while(0)
static int uuid_valid(const char *value)
{
    if(strlen(value)!=36) return 0;
    for(unsigned i=0;i<36;++i) if(i==8 || i==13 || i==18 || i==23) { if(value[i]!='-') return 0; } else if(!strchr("0123456789abcdefABCDEF",value[i])) return 0;
    return 1;
}
static void hash_integer(gcry_md_hd_t hash,const uint8_t *data,size_t length)
{ while(length>1 && !*data) { ++data; --length; } gcry_md_write(hash,data,length); }
static int proof_compute(struct capture *capture,unsigned direction,uint8_t result[64])
{
    REQUIRE(capture->direct_count==DIRECT_COUNT && capture->salt_len && capture->a_len && capture->b_len);
    uint8_t padded[768]={0},scramble[64]; memcpy(padded+384-capture->a_len,capture->public_a,capture->a_len); memcpy(padded+768-capture->b_len,capture->public_b,capture->b_len); gcry_md_hash_buffer(GCRY_MD_SHA512,scramble,padded,sizeof(padded)); REQUIRE(!memcmp(scramble,capture->direct[DIRECT_SCRAMBLE],64));
    uint8_t xor_hash[64],client[64]; for(unsigned i=0;i<64;++i) xor_hash[i]=capture->direct[DIRECT_MODULUS][i]^capture->direct[DIRECT_GENERATOR][i];
    gcry_md_hd_t hash; REQUIRE(!gcry_md_open(&hash,GCRY_MD_SHA512,0));
    gcry_md_write(hash,xor_hash,64); gcry_md_write(hash,capture->direct[DIRECT_IDENTITY],64);
    hash_integer(hash,capture->salt,capture->salt_len); hash_integer(hash,capture->public_a,capture->a_len); hash_integer(hash,capture->public_b,capture->b_len); gcry_md_write(hash,capture->direct[DIRECT_SESSION],64);
    memcpy(client,gcry_md_read(hash,0),64); gcry_md_close(hash);
    if(direction==1) { memcpy(result,client,64); return 0; }
    REQUIRE(direction==2 && capture->client_proof && !gcry_md_open(&hash,GCRY_MD_SHA512,0));
    hash_integer(hash,capture->public_a,capture->a_len); gcry_md_write(hash,client,64); gcry_md_write(hash,capture->direct[DIRECT_SESSION],64); memcpy(result,gcry_md_read(hash,0),64); gcry_md_close(hash); return 0;
}
static int direct_hash(struct capture *capture,const uint8_t *digest,size_t length)
{
    REQUIRE(length==64 && capture->direct_count<DIRECT_COUNT && capture->pairs==1);
    uint8_t expected[64],modulus[384],generator=5;
    REQUIRE(strlen(srp_modulus_hex)==sizeof(modulus)*2); for(unsigned i=0;i<sizeof(modulus);++i) { char hex[3]={srp_modulus_hex[i*2],srp_modulus_hex[i*2+1],0}; modulus[i]=(uint8_t)strtoul(hex,NULL,16); }
    if(capture->direct_count==DIRECT_MULTIPLIER) { uint8_t padded[768]={0}; memcpy(padded,modulus,384); padded[767]=5; gcry_md_hash_buffer(GCRY_MD_SHA512,expected,padded,sizeof(padded)); }
    else if(capture->direct_count==DIRECT_PASSWORD) { REQUIRE(capture->salt_len); uint8_t input[80],password[64]; gcry_md_hash_buffer(GCRY_MD_SHA512,password,"Pair-Setup:3939",15); size_t offset=0; while(offset+1<capture->salt_len && !capture->salt[offset]) ++offset; size_t salt_length=capture->salt_len-offset; memcpy(input,capture->salt+offset,salt_length); memcpy(input+salt_length,password,64); gcry_md_hash_buffer(GCRY_MD_SHA512,expected,input,salt_length+64); }
    else if(capture->direct_count==DIRECT_MODULUS) gcry_md_hash_buffer(GCRY_MD_SHA512,expected,modulus,sizeof(modulus));
    else if(capture->direct_count==DIRECT_GENERATOR) gcry_md_hash_buffer(GCRY_MD_SHA512,expected,&generator,1);
    else if(capture->direct_count==DIRECT_IDENTITY) gcry_md_hash_buffer(GCRY_MD_SHA512,expected,"Pair-Setup",10);
    if(capture->direct_count!=DIRECT_SCRAMBLE && capture->direct_count!=DIRECT_SESSION) { if(memcmp(expected,digest,64)) fprintf(stderr,"operation=direct-hash-purpose index=%zu\n",capture->direct_count); REQUIRE(!memcmp(expected,digest,64)); }
    memcpy(capture->direct[capture->direct_count++],digest,64); return 0;
}
static int field(struct trace_event *event,const char *name,enum trace_semantics semantics,unsigned domain,const void *data,size_t length)
{
    REQUIRE(event->count<TRACE_MAX_FIELDS && strlen(name)<sizeof(event->fields[0].name) && length<=TRACE_VALUE_BYTES);
    struct trace_field *value=&event->fields[event->count++]; strcpy(value->name,name); value->semantics=semantics; value->domain=domain; value->length=length; memcpy(value->value,data,length); return 0;
}
static int number(struct trace_event *event,const char *name,enum trace_semantics semantics,unsigned domain,uint64_t value)
{
    uint8_t bytes[8]; for(unsigned i=0;i<8;++i) bytes[7-i]=(uint8_t)(value>>(i*8));
    return field(event,name,semantics,domain,bytes,sizeof(bytes));
}
static int tlv_field(struct trace_event *event,const char *name,unsigned type,const uint8_t *value,size_t length)
{
    unsigned domain=type==2 ? 16 : type==3 ? 17 : type==4 ? 18 : 0;
    if(type==2 || type==3) { uint8_t padded[384]={0}; size_t width=type==2 ? 16 : 384; REQUIRE(length && length<=width); memcpy(padded+width-length,value,length); return field(event,name,TRACE_IDENTITY,domain,padded,width); }
    return field(event,name,domain ? TRACE_IDENTITY : TRACE_EXACT,domain,value,length);
}
static int active_remote_field(struct trace_event *event,const char *value)
{
    REQUIRE(*value && strlen(value)<=10 && strspn(value,"0123456789")==strlen(value));
    char *tail; unsigned long parsed=strtoul(value,&tail,10);
    REQUIRE(!*tail && parsed<=UINT32_MAX);
    return number(event,"Active-Remote",TRACE_IDENTITY,14,parsed);
}
static int plist_fields(struct capture *capture,struct trace_event *event,plist_t node,const char *path)
{
    plist_type type=plist_get_node_type(node); char nested[TRACE_PATH_BYTES];
    if(type==PLIST_DICT) {
        plist_dict_iter iterator=NULL; plist_dict_new_iter(node,&iterator); REQUIRE(iterator);
        for(;;) { char *key=NULL; plist_t child=NULL; plist_dict_next_item(node,iterator,&key,&child); if(!child) break;
            int size=snprintf(nested,sizeof(nested),"%s%s%s",path,*path ? "." : "",key);
            if(size<=0 || (size_t)size>=sizeof(nested)) fprintf(stderr,"operation=plist-path parent=%s key=%s required=%d capacity=%zu\n",path,key,size,sizeof(nested));
            free(key); REQUIRE(size>0 && (size_t)size<sizeof(nested));
            if(plist_fields(capture,event,child,nested)) { free(iterator); return -1; }
        } free(iterator); return 0;
    }
    if(type==PLIST_ARRAY) {
        REQUIRE(!number(event,path,TRACE_EXACT,0,plist_array_get_size(node)));
        for(uint32_t i=0;i<plist_array_get_size(node);++i) { int n=snprintf(nested,sizeof(nested),"%s[%u]",path,i); REQUIRE(n>0 && (size_t)n<sizeof(nested)); REQUIRE(!plist_fields(capture,event,plist_array_get_item(node,i),nested)); }
        return 0;
    }
    const char *leaf=strrchr(path,'.'); leaf=leaf ? leaf+1 : path;
    unsigned domain=0; enum trace_semantics semantics=TRACE_EXACT;
    if(!strcmp(leaf,"sessionUUID")) { domain=2; semantics=TRACE_IDENTITY; }
    if(!strcmp(leaf,"groupUUID")) { domain=3; semantics=TRACE_IDENTITY; }
    if(!strcmp(leaf,"ClockID")) { domain=23; semantics=TRACE_IDENTITY; }
    if(!strcmp(leaf,"macAddress")) { domain=24; semantics=TRACE_IDENTITY; }
    if(!strcmp(leaf,"streamConnectionID")) { domain=1; semantics=TRACE_IDENTITY; }
    if(!strcmp(leaf,"deviceID") || !strcmp(leaf,"deviceid")) { domain=5; semantics=TRACE_IDENTITY; }
    if(!strcmp(leaf,"controlPort")) { domain=6; semantics=TRACE_IDENTITY; }
    if(!strcmp(leaf,"eventPort")) { domain=7; semantics=TRACE_IDENTITY; }
    if(!strcmp(leaf,"dataPort")) { domain=8; semantics=TRACE_IDENTITY; }
    if(!strcmp(leaf,"ID") && event->direction==1) { domain=9; semantics=TRACE_IDENTITY; }
    if(!strcmp(leaf,"shk")) { domain=12; semantics=TRACE_IDENTITY; }
    if(!strcmp(leaf,"pi") || !strcmp(leaf,"gid")) { domain=27; semantics=TRACE_IDENTITY; }
    if(!strcmp(leaf,"psi")) { domain=29; semantics=TRACE_IDENTITY; }
    if(!strcmp(leaf,"pgid")) { domain=30; semantics=TRACE_IDENTITY; }
    if(!strcmp(leaf,"pk")) { domain=28; semantics=TRACE_IDENTITY; }
    if(type==PLIST_UINT) { uint64_t value; plist_get_uint_val(node,&value); if(!strcmp(leaf,"ClockID") && event->direction==1) capture->clock_id=value; if(domain==6 || domain==7 || domain==8) REQUIRE(value && value<=65535); return number(event,path,semantics,domain,value); }
    if(type==PLIST_BOOLEAN) { uint8_t value; plist_get_bool_val(node,&value); return field(event,path,TRACE_EXACT,0,&value,1); }
    if(type==PLIST_REAL) { double value; uint64_t bits; plist_get_real_val(node,&value); memcpy(&bits,&value,sizeof(bits)); return number(event,path,TRACE_EXACT,0,bits); }
    if(type==PLIST_STRING) { char *value=NULL; plist_get_string_val(node,&value); REQUIRE(value);
        if(domain==2 || domain==3 || domain==9 || domain==27 || domain==29 || domain==30) REQUIRE(uuid_valid(value));
        if(domain==28) { REQUIRE(strlen(value)==64); uint8_t decoded[32]; for(unsigned i=0;i<32;++i) { char hex[3]={value[i*2],value[i*2+1],0},*tail; REQUIRE(strspn(hex,"0123456789abcdefABCDEF")==2); decoded[i]=(uint8_t)strtoul(hex,&tail,16); REQUIRE(!*tail); } int result=field(event,path,semantics,domain,decoded,sizeof(decoded)); free(value); return result; }
        if(!strcmp(leaf,"senderAddress")) { char *colon=strrchr(value,':'); REQUIRE(colon && colon[1]); char *tail; unsigned long port=strtoul(colon+1,&tail,10); REQUIRE(!*tail && port && port<=65535); *colon=0; uint8_t address[16],digest[32]; int family=inet_pton(AF_INET,value,address)==1 ? AF_INET : inet_pton(AF_INET6,value,address)==1 ? AF_INET6 : 0; REQUIRE(family); REQUIRE(!number(event,"senderAddress-family",TRACE_EXACT,0,(unsigned)family) && !number(event,"senderAddress-port",TRACE_IDENTITY,26,port)); gcry_md_hash_buffer(GCRY_MD_SHA256,digest,value,strlen(value)); int result=field(event,path,TRACE_IDENTITY,25,digest,sizeof(digest)); free(value); return result; }
        if(strstr(path,"Addresses[") || path[0]=='[') {
            uint8_t address[16],digest[32]; int family=inet_pton(AF_INET,value,address)==1 ? AF_INET : inet_pton(AF_INET6,value,address)==1 ? AF_INET6 : 0; REQUIRE(family);
            char family_name[TRACE_PATH_BYTES]; int n=snprintf(family_name,sizeof(family_name),"%s-family",path); REQUIRE(n>0 && (size_t)n<sizeof(family_name)); REQUIRE(!number(event,family_name,TRACE_EXACT,0,(unsigned)family));
            gcry_md_hash_buffer(GCRY_MD_SHA256,digest,value,strlen(value)); int result=field(event,path,TRACE_IDENTITY,25,digest,sizeof(digest)); free(value); return result;
        }
        int result=field(event,path,semantics,domain,value,strlen(value)); free(value); return result; }
    if(type==PLIST_DATA) { char *value=NULL; uint64_t length=0; plist_get_data_val(node,&value,&length); REQUIRE(value);
        if(!strcmp(leaf,"txtAirPlay")) { plist_t entries=plist_new_dict(); REQUIRE(entries); size_t offset=0; while(offset<length) { unsigned size=(uint8_t)value[offset++]; REQUIRE(size && size<=length-offset); char entry[256]; memcpy(entry,value+offset,size); entry[size]=0; REQUIRE(strlen(entry)==size); char *equal=strchr(entry,'='); REQUIRE(equal && equal!=entry); *equal=0; REQUIRE(!plist_dict_get_item(entries,entry)); plist_dict_set_item(entries,entry,plist_new_string(equal+1)); offset+=size; } int result=plist_fields(capture,event,entries,path); plist_free(entries); free(value); return result; }
        if(domain==28) REQUIRE(length==32);
        if(!strcmp(leaf,"shk")) { REQUIRE(length==32 && capture->server_proof && capture->direct_count==DIRECT_COUNT && !memcmp(value,capture->direct[DIRECT_SESSION],32)); memcpy(capture->key,value,32); capture->have_key=1; }
        int result=field(event,path,semantics,domain,value,(size_t)length); free(value); return result;
    }
    fprintf(stderr,"operation=plist-type path=%s type=%d\n",path,(int)type); REQUIRE(0); return -1;
}
static int rtsp(struct capture *capture,unsigned direction,char *data,size_t length)
{
    if(direction==2 && (length<9 || memcmp(data,"RTSP/1.0 ",9))) return 0;
    char *separator=memmem(data,length,"\r\n\r\n",4); REQUIRE(separator);
    size_t header_length=(size_t)(separator-data)+4; uint8_t *body=(uint8_t *)data+header_length; size_t body_length=length-header_length;
    unsigned cseq_count=0,length_count=0; unsigned long cseq=0;
    char *header=strstr(data,"\r\n"); REQUIRE(header && header<=separator); header+=2;
    while(header<separator) {
        char *end=strstr(header,"\r\n"); REQUIRE(end && end<=separator);
        if(!strncmp(header,"CSeq: ",6)) { char *tail; cseq=strtoul(header+6,&tail,10); REQUIRE(tail==end && ++cseq_count==1); }
        if(!strncmp(header,"Content-Length: ",16)) { char *tail; unsigned long n=strtoul(header+16,&tail,10); REQUIRE(tail==end && n==body_length && ++length_count==1); }
        header=end+2;
    }
    REQUIRE(cseq_count==1 && (!body_length || length_count==1));
    if(direction==1) { int discovery_probe=length>=10 && !memcmp(data,"GET /info ",10) && !capture->pairs && capture->discovery<2 && cseq==1; REQUIRE(!capture->pending && (!capture->have_cseq || cseq==capture->request_cseq+1 || discovery_probe)); capture->request_cseq=cseq; capture->have_cseq=1; capture->pending=1; }
    else { REQUIRE(capture->pending && cseq==capture->request_cseq); capture->pending=0; }
    char *line_end=strstr(data,"\r\n"); REQUIRE(line_end); *line_end=0;
    struct trace_event event={.direction=direction}; char *cursor=data;
    if(direction==1) {
        char method[32],url[256],version[16]; REQUIRE(sscanf(data,"%31s %255s %15s",method,url,version)==3 && !strcmp(version,"RTSP/1.0"));
        if(!strcmp(method,"GET") || !strcmp(url,"/feedback")) {
            REQUIRE((!strcmp(method,"GET") && !strcmp(url,"/info")) || (!strcmp(method,"POST") && !strcmp(url,"/feedback")));
            REQUIRE(!capture->maintenance_response && !body_length);
            capture->maintenance_response=!strcmp(method,"GET") ? 1 : 2; ++capture->maintenance_requests; return 0;
        }
        strcpy(event.operation,method);
        if(!strcmp(method,"RECORD")) capture->have_record=1;
        if(!strncmp(url,"rtsp://",7)) { const char *id=strrchr(url,'/'); REQUIRE(id && id[1]); char *end; unsigned long long value=strtoull(id+1,&end,10); REQUIRE(!*end); REQUIRE(!number(&event,"url-session",TRACE_IDENTITY,1,value)); }
        else REQUIRE(!field(&event,"uri",TRACE_EXACT,0,url,strlen(url)));
    } else {
        if(capture->maintenance_response) {
            unsigned status; REQUIRE(sscanf(data,"RTSP/1.0 %u",&status)==1 && status==200);
            if(capture->maintenance_response==1) {
                plist_t info=NULL; REQUIRE(body_length>=8 && !memcmp(body,"bplist00",8)); plist_from_bin((const char *)body,(uint32_t)body_length,&info); REQUIRE(info);
                uint64_t features=0; plist_t value=plist_dict_get_item(info,"features"); REQUIRE(value && plist_get_node_type(value)==PLIST_UINT); plist_get_uint_val(value,&features); REQUIRE(features==UINT64_C(0x00018340405C4A00));
                struct trace_event discovery={.direction=2}; strcpy(discovery.operation,"receiver-info"); REQUIRE(!plist_fields(capture,&discovery,info,""));
                if(!capture->info.count) REQUIRE(!trace_append(&capture->info,&discovery));
                else { struct protocol_trace *repeat=calloc(1,sizeof(*repeat)); REQUIRE(repeat && !trace_append(repeat,&discovery)); struct trace_difference difference; int same=trace_compare(&capture->info,repeat,&difference); if(same) fprintf(stderr,"operation=repeat-info field=%s reason=%s\n",difference.field,difference.reason); free(repeat); REQUIRE(!same); }
                plist_free(info); ++capture->discovery;
            } else { plist_t feedback=NULL; REQUIRE(body_length>=8 && !memcmp(body,"bplist00",8)); plist_from_bin((const char *)body,(uint32_t)body_length,&feedback); REQUIRE(feedback && plist_get_node_type(feedback)==PLIST_DICT); plist_free(feedback); ++capture->feedback; }
            capture->maintenance_response=0; return 0;
        }
        if(strncmp(data,"RTSP/1.0 ",9)) return 0;
        strcpy(event.operation,"response"); unsigned status; REQUIRE(sscanf(data,"RTSP/1.0 %u",&status)==1 && status==200); REQUIRE(!number(&event,"status",TRACE_EXACT,0,status));
        if(!capture->pairs) return 0;
    }
    event.state=capture->pairs;
    cursor=line_end+2;
    while(cursor<separator) {
        char *end=strstr(cursor,"\r\n"); REQUIRE(end && end<=separator); *end=0; char *colon=strchr(cursor,':'); REQUIRE(colon); *colon=0; char *value=colon+1; while(*value==' ') ++value;
        if(!strcmp(cursor,"Content-Length")) { char *tail; unsigned long n=strtoul(value,&tail,10); REQUIRE(!*tail && n==body_length); }
        else if(!strcmp(cursor,"CSeq")) { char *tail; unsigned long n=strtoul(value,&tail,10); REQUIRE(!*tail && n>=capture->maintenance_requests); REQUIRE(!number(&event,cursor,TRACE_SEQUENCE,10,n-capture->maintenance_requests)); }
        else if(!strcmp(cursor,"DACP-ID") || !strcmp(cursor,"Client-Instance")) { REQUIRE(!field(&event,cursor,TRACE_IDENTITY,11,value,strlen(value))); }
        else if(!strcmp(cursor,"Active-Remote")) { REQUIRE(!active_remote_field(&event,value)); }
        else if(!strcmp(cursor,"Date")) { struct tm date={0}; char *tail=strptime(value,"%a, %d %b %Y %H:%M:%S GMT",&date); REQUIRE(tail && !*tail); time_t stamp=timegm(&date); REQUIRE(stamp>0 && (!capture->first_date || (stamp>=capture->last_date && stamp-capture->first_date<=90))); if(!capture->first_date) capture->first_date=stamp; capture->last_date=stamp; REQUIRE(!number(&event,"Date-valid-monotonic",TRACE_EXACT,0,1)); }
        else if(!strcmp(cursor,"RTP-Info")) { unsigned sequence,timestamp; int consumed=0; REQUIRE(sscanf(value,"seq=%u;rtptime=%u%n",&sequence,&timestamp,&consumed)==2 && !value[consumed] && sequence<=UINT16_MAX); REQUIRE(!number(&event,"RTP-Info-sequence",TRACE_SEQUENCE,20,sequence) && !number(&event,"RTP-Info-time",TRACE_TIME,21,timestamp)); if(direction==1 && !strcmp(event.operation,"RECORD")) { capture->record_sequence=sequence; capture->record_time=timestamp; capture->have_record=1; } }
        else REQUIRE(!field(&event,cursor,TRACE_EXACT,0,value,strlen(value)));
        cursor=end+2;
    }
    if(body_length>=8 && !memcmp(body,"bplist00",8)) { plist_t node=NULL; plist_from_bin((const char *)body,(uint32_t)body_length,&node); REQUIRE(node); int result=plist_fields(capture,&event,node,""); plist_free(node); REQUIRE(!result); }
    else if(body_length && (!strcmp(event.operation,"POST") || (direction==2 && capture->pairs<5))) {
        pair_tlv_values_t *values=pair_tlv_new(); REQUIRE(values && !pair_tlv_parse(body,body_length,values));
        for(pair_tlv_t *value=values->head;value;value=value->next) {
            char name[32]; snprintf(name,sizeof(name),"tlv-%u",value->type);
            if(value->type==2) { REQUIRE(direction==2 && value->size && value->size<=sizeof(capture->salt)); memcpy(capture->salt,value->value,value->size); capture->salt_len=value->size; }
            if(value->type==3) { REQUIRE(value->size && value->size<=sizeof(capture->public_a)); if(direction==1) { memcpy(capture->public_a,value->value,value->size); capture->a_len=value->size; } else { memcpy(capture->public_b,value->value,value->size); capture->b_len=value->size; } }
            if(value->type==4) { uint8_t expected[64]; REQUIRE(value->size==64 && !proof_compute(capture,direction,expected) && !memcmp(value->value,expected,64)); if(direction==1) capture->client_proof=1; else capture->server_proof=1; }
            REQUIRE(!tlv_field(&event,name,value->type,value->value,value->size));
        } pair_tlv_free(values);
    } else if(body_length) REQUIRE(!field(&event,"body",TRACE_EXACT,0,body,body_length));
    if(direction==1 && !strcmp(event.operation,"POST")) ++capture->pairs;
    if(direction==1 && !strcmp(event.operation,"SETUP")) capture->authenticated=1;
    REQUIRE(!trace_append(&capture->control,&event)); return 0;
}
static int audio(struct capture *capture,const uint8_t *packet,size_t length)
{
    REQUIRE(capture->have_key && length>36);
    gcry_cipher_hd_t cipher; REQUIRE(!gcry_cipher_open(&cipher,GCRY_CIPHER_CHACHA20,GCRY_CIPHER_MODE_POLY1305,0));
    uint8_t nonce[12]={0},plain[65536],digest[32]; memcpy(nonce+4,packet+length-8,8);
    size_t payload=length-12-24;
    REQUIRE(!gcry_cipher_setkey(cipher,capture->key,32) && !gcry_cipher_setiv(cipher,nonce,12) && !gcry_cipher_authenticate(cipher,packet+4,8));
    REQUIRE(!gcry_cipher_decrypt(cipher,plain,sizeof(plain),packet+12,payload) && !gcry_cipher_checktag(cipher,packet+12+payload,16)); gcry_cipher_close(cipher);
    gcry_md_hash_buffer(GCRY_MD_SHA256,digest,plain,payload);
    REQUIRE(capture->packets<AUDIO_PACKET_LIMIT && capture->have_record);
    struct audio_sample *sample=&capture->samples[capture->packets];
    sample->sequence=(uint16_t)((unsigned)packet[2]<<8|packet[3]); sample->timestamp=(uint32_t)packet[4]<<24|(uint32_t)packet[5]<<16|(uint32_t)packet[6]<<8|packet[7];
    if(!capture->packets) { REQUIRE(capture->sync_count && capture->record_time==sample->timestamp); capture->record_sequence=sample->sequence; }
    memcpy(sample->flags,packet,2); memcpy(sample->ssrc,packet+8,4); memcpy(sample->digest,digest,32); sample->length=payload;
    REQUIRE(packet[0]==0x80 && packet[1]==(capture->packets ? 0x60 : 0xe0));
    REQUIRE((uint16_t)(sample->sequence-capture->record_sequence)==(uint16_t)capture->packets && (uint32_t)(sample->timestamp-capture->record_time)==capture->packets*352);
    REQUIRE(!capture->packets || !memcmp(sample->ssrc,capture->samples[0].ssrc,4));
    uint8_t expected_nonce[8]={0}; expected_nonce[0]=(uint8_t)sample->sequence; expected_nonce[1]=(uint8_t)(sample->sequence>>8); REQUIRE(!memcmp(expected_nonce,packet+length-8,8));
    AVPacket *encoded=av_packet_alloc(); AVFrame *frame=av_frame_alloc(); REQUIRE(encoded && frame && !av_new_packet(encoded,(int)payload)); memcpy(encoded->data,plain,payload);
    REQUIRE(!avcodec_send_packet(capture->decoder,encoded) && !avcodec_receive_frame(capture->decoder,frame) && frame->nb_samples==352 && frame->format==AV_SAMPLE_FMT_S16P);
    uint8_t pcm[1408]; for(unsigned i=0;i<352;++i) { memcpy(pcm+i*4,frame->data[0]+i*2,2); memcpy(pcm+i*4+2,frame->data[1]+i*2,2); }
    REQUIRE(write(capture->pcm,pcm,sizeof(pcm))==(ssize_t)sizeof(pcm));
    struct media_quality quality={44100,16,2,0}; struct evbuffer *reencoded=evbuffer_new(); REQUIRE(reencoded);
    REQUIRE(codec_alac_encode(reencoded,capture->encoder,pcm,sizeof(pcm),352,&quality)==(int)payload && !memcmp(evbuffer_pullup(reencoded,-1),plain,payload));
    evbuffer_free(reencoded); av_frame_free(&frame); av_packet_free(&encoded);
    ++capture->packets; return 0;
}
static int read_capture(const char *directory,struct capture *capture)
{
    int descriptor=open(directory,O_RDONLY|O_NOFOLLOW); REQUIRE(descriptor>=0); struct stat metadata; REQUIRE(!fstat(descriptor,&metadata) && S_ISREG(metadata.st_mode) && metadata.st_uid==geteuid() && (metadata.st_mode&0777)==0600);
    char parent[2048]; REQUIRE(strlen(directory)<sizeof(parent)); strcpy(parent,directory); char *slash=strrchr(parent,'/'); REQUIRE(slash && slash!=parent); *slash=0; REQUIRE(!stat(parent,&metadata) && S_ISDIR(metadata.st_mode) && metadata.st_uid==geteuid() && (metadata.st_mode&0777)==0700);
    FILE *input=fdopen(descriptor,"rb"); REQUIRE(input); uint32_t header[2];
    capture->samples=calloc(AUDIO_PACKET_LIMIT,sizeof(*capture->samples)); REQUIRE(capture->samples);
    char pcm_path[2048]; int n=snprintf(pcm_path,sizeof(pcm_path),"%s.decoded.pcm",directory); REQUIRE(n>0 && (size_t)n<sizeof(pcm_path)); capture->pcm=open(pcm_path,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600); REQUIRE(capture->pcm>=0);
    struct media_quality quality={44100,16,2,0}; capture->encoder=codec_alac_new(&quality); REQUIRE(capture->encoder);
    capture->decoder=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_ALAC)); REQUIRE(capture->decoder);
    capture->decoder->extradata_size=capture->encoder->context->extradata_size; capture->decoder->extradata=av_mallocz((size_t)capture->decoder->extradata_size+AV_INPUT_BUFFER_PADDING_SIZE); REQUIRE(capture->decoder->extradata); memcpy(capture->decoder->extradata,capture->encoder->context->extradata,(size_t)capture->decoder->extradata_size); REQUIRE(avcodec_open2(capture->decoder,avcodec_find_decoder(AV_CODEC_ID_ALAC),NULL)>=0);
    for(;;) {
        size_t size=fread(header,1,sizeof(header),input); if(!size) break; REQUIRE(size==sizeof(header) && header[0]>=1 && header[0]<=6 && header[0]!=5);
        REQUIRE(header[1] && header[1]<=65536); char *data=calloc(1,(size_t)header[1]+1); REQUIRE(data && fread(data,1,header[1],input)==header[1]);
        int result=0;
        if(header[0]==6) result=direct_hash(capture,(uint8_t *)data,header[1]);
        else if(header[0]==4) {
            REQUIRE(header[1]==28 && capture->clock_id); uint64_t ns=0,clock=0; uint32_t pos=0,rtptime=0;
            for(unsigned i=0;i<8;++i) { ns=(ns<<8)|(uint8_t)data[8+i]; clock=(clock<<8)|(uint8_t)data[20+i]; }
            for(unsigned i=0;i<4;++i) { pos=(pos<<8)|(uint8_t)data[4+i]; rtptime=(rtptime<<8)|(uint8_t)data[16+i]; }
            REQUIRE((uint8_t)data[0]==(capture->sync_count ? 0x80 : 0x90) && (uint8_t)data[1]==0xd7 && !data[2] && data[3]==6 && clock==capture->clock_id);
            uint32_t next=rtptime+11025;
            /* timestamp_set runs before adding input and encoding the next packet. */
            REQUIRE((uint32_t)(pos-next+77175)<352);
            if(!capture->sync_count) capture->record_time=next;
            REQUIRE(next==(uint32_t)(capture->record_time+capture->packets*352));
            if(capture->sync_count) { REQUIRE(ns>capture->last_sync_ns); uint64_t elapsed=ns-capture->last_sync_ns; uint64_t samples=(uint32_t)(pos-capture->last_sync_pos); uint64_t expected=samples*UINT64_C(1000000000)/44100; uint64_t error=elapsed>expected ? elapsed-expected : expected-elapsed; REQUIRE(error<UINT64_C(20000000)); }
            capture->last_sync_ns=ns; capture->last_sync_pos=pos; ++capture->sync_count;
        } else result=header[0]==3 ? audio(capture,(uint8_t *)data,header[1]) : rtsp(capture,header[0],data,header[1]);
        free(data); REQUIRE(!result);
    } REQUIRE(feof(input) && !ferror(input)); fclose(input); REQUIRE(!close(capture->pcm)); REQUIRE(capture->authenticated && capture->packets*352>=1323000 && !capture->maintenance_response && !capture->pending && capture->sync_count>=16);
    static const char *operations[]={"POST","POST","SETUP","RECORD","SETPEERS","SETUP","SET_PARAMETER","TEARDOWN"};
    REQUIRE(capture->control.count==sizeof(operations)/sizeof(*operations)*2);
    for(size_t i=0;i<sizeof(operations)/sizeof(*operations);++i) REQUIRE(capture->control.events[i*2].direction==1 && !strcmp(capture->control.events[i*2].operation,operations[i]) && capture->control.events[i*2+1].direction==2 && !strcmp(capture->control.events[i*2+1].operation,"response"));
    return 0;
}
static int parser_self_test(void)
{
    struct capture *capture=calloc(1,sizeof(*capture)); REQUIRE(capture);
    struct trace_event event={.direction=1}; strcpy(event.operation,"SETUP");
    plist_t node=plist_new_dict(),nested=plist_new_dict(); REQUIRE(node && nested);
    plist_dict_set_item(nested,"SupportsClockPortMatchingOverride",plist_new_bool(1)); plist_dict_set_item(node,"timingPeerInfo",nested);
    REQUIRE(!plist_fields(capture,&event,node,"") && event.count==1 && !strcmp(event.fields[0].name,"timingPeerInfo.SupportsClockPortMatchingOverride")); plist_free(node);
    REQUIRE(uuid_valid("12345678-1234-5678-90ab-123456789abc") && !uuid_valid("malformed-identity"));
    struct trace_event short_number={0},full_number={0}; uint8_t short_value=5,full_value[384]={0}; full_value[383]=5;
    REQUIRE(!tlv_field(&short_number,"tlv-3",3,&short_value,1) && !tlv_field(&full_number,"tlv-3",3,full_value,sizeof(full_value)) && short_number.fields[0].length==full_number.fields[0].length && !memcmp(short_number.fields[0].value,full_number.fields[0].value,384));
    short_number.count=full_number.count=0;
    REQUIRE(!active_remote_field(&short_number,"5") && !active_remote_field(&full_number,"4294967295") && short_number.fields[0].length==8 && full_number.fields[0].length==8);
    REQUIRE(active_remote_field(&short_number,"4294967296")<0 && active_remote_field(&short_number,"-1")<0 && active_remote_field(&short_number,"")<0);
    char truncated[]="RECORD rtsp://127.0.0.1/1 RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 4\r\n\r\nx";
    REQUIRE(rtsp(capture,1,truncated,sizeof(truncated)-1)<0);
    char duplicate[]="RECORD rtsp://127.0.0.1/1 RTSP/1.0\r\nCSeq: 1\r\nCSeq: 1\r\n\r\n";
    REQUIRE(rtsp(capture,1,duplicate,sizeof(duplicate)-1)<0);
    memset(capture,0,sizeof(*capture)); capture->direct_count=DIRECT_COUNT; memset(capture->direct,0x3f,sizeof(capture->direct)); capture->salt_len=capture->a_len=capture->b_len=1; capture->salt[0]=1; capture->public_a[0]=2; capture->public_b[0]=3;
    uint8_t padded[768]={0}; padded[383]=2; padded[767]=3; gcry_md_hash_buffer(GCRY_MD_SHA512,capture->direct[DIRECT_SCRAMBLE],padded,sizeof(padded));
    uint8_t client[64]; REQUIRE(!proof_compute(capture,1,client));
    char proof[256]; int header=snprintf(proof,sizeof(proof),"POST /pair-setup RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 66\r\n\r\n"); REQUIRE(header>0 && header+66<(int)sizeof(proof));
    proof[header]=4; proof[header+1]=64; memcpy(proof+header+2,client,64); proof[header+66]=0;
    REQUIRE(!rtsp(capture,1,proof,(size_t)header+66));
    capture->pending=capture->have_cseq=capture->client_proof=0; capture->control.count=0;
    header=snprintf(proof,sizeof(proof),"POST /pair-setup RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 66\r\n\r\n"); proof[header]=4; proof[header+1]=64; memcpy(proof+header+2,client,64); proof[header+2]^=1; proof[header+66]=0;
    REQUIRE(rtsp(capture,1,proof,(size_t)header+66)<0);
    capture->server_proof=1; node=plist_new_data((const char *)client,32); REQUIRE(node); event.count=0; REQUIRE(plist_fields(capture,&event,node,"shk")<0); plist_free(node);
    capture->pending=1; capture->request_cseq=1; capture->client_proof=1; header=snprintf(proof,sizeof(proof),"RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 66\r\n\r\n"); proof[header]=4; proof[header+1]=64; memcpy(proof+header+2,client,64); proof[header+66]=0; REQUIRE(rtsp(capture,2,proof,(size_t)header+66)<0);
    free(capture); puts("capture parser preserves nested paths and rejects malformed length, duplicate sequence, invalid identity, unbound proof, exchanged proof roles and proof used as audio material"); return 0;
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--self-test")) return gcry_check_version(NULL) && !parser_self_test() ? 0 : 1;
    if(argc!=3 || !gcry_check_version(NULL)) return 2;
    struct capture *reference=calloc(1,sizeof(*reference)),*candidate=calloc(1,sizeof(*candidate)); if(!reference || !candidate) return 2;
    if(read_capture(argv[1],reference) || read_capture(argv[2],candidate)) return 1;
    struct trace_difference difference;
    printf("reference_control=%zu candidate_control=%zu reference_audio=%u candidate_audio=%u reference_discovery=%u candidate_discovery=%u reference_feedback=%u candidate_feedback=%u\n",reference->control.count,candidate->control.count,reference->packets,candidate->packets,reference->discovery,candidate->discovery,reference->feedback,candidate->feedback);
    int result=trace_compare(&reference->control,&candidate->control,&difference);
    if(!result) result=trace_compare(&reference->info,&candidate->info,&difference);
    if(result) fprintf(stderr,"operation=control-compare event=%zu field=%s reason=%s\n",difference.event,difference.field,difference.reason);
    if(!result) {
        REQUIRE(reference->discovery==2 && candidate->discovery==1
                && reference->feedback>0 && reference->feedback<=2
                && candidate->feedback>0 && candidate->feedback<=2);
        size_t corresponding=(1323000+351)/352;
        for(size_t i=0;i<corresponding;++i) {
            const struct audio_sample *left=&reference->samples[i],*right=&candidate->samples[i];
            if(left->length!=right->length || memcmp(left->digest,right->digest,32)) { fprintf(stderr,"operation=audio-compare sample-offset=%zu field=alac-payload reason=corresponding-payload-differs\n",i*352); result=1; break; }
        }
        if(!result) puts("all audio datagrams authenticated; nonce sequence timestamp marker and stable SSRC relationships verified; corresponding full-input ALAC payloads equal and every decoded packet re-encodes identically");
    }
    codec_alac_free(reference->encoder); codec_alac_free(candidate->encoder); avcodec_free_context(&reference->decoder); avcodec_free_context(&candidate->decoder); free(reference->samples); free(candidate->samples); sodium_memzero(reference,sizeof(*reference)); sodium_memzero(candidate,sizeof(*candidate)); free(reference); free(candidate); return result ? 1 : 0;
}
