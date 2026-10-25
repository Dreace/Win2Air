#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gcrypt.h>
#include <sodium.h>
#include "pair.h"
#include "pair-tlv.h"
static unsigned long counter;
static uint8_t peer_key[32];
static const char *random_name(void) { return "public-pairing-test-stream"; }
static void random_fill(void *const output,size_t length)
{
    uint8_t seed[randombytes_SEEDBYTES]={0};
    unsigned long value=++counter;
    for (size_t i=0;i<sizeof(value);++i) seed[i]=(uint8_t)(value>>(i*8));
    seed[31]=0x75;
    randombytes_buf_deterministic(output,length,seed);
}
static uint32_t random_word(void) { uint32_t value; random_fill(&value,sizeof(value)); return value; }
void __wrap_gcry_mpi_randomize(gcry_mpi_t target,unsigned bits,enum gcry_random_level level)
{
    uint8_t bytes[512]; gcry_mpi_t value=NULL; size_t length=(bits+7)/8;
    (void)level; assert(bits && length<=sizeof(bytes)); random_fill(bytes,length);
    if (bits%8) bytes[0]&=(uint8_t)((1u<<(bits%8))-1);
    assert(!gcry_mpi_scan(&value,GCRYMPI_FMT_USG,bytes,length,NULL));
    gcry_mpi_set(target,value); gcry_mpi_release(value);
}
static void emit(const char *name,const void *data,size_t length)
{
    const uint8_t *bytes=data; printf("%s %zu ",name,length);
    for(size_t i=0;i<length;++i) printf("%02x",bytes[i]);
    putchar('\n');
}
static int save(uint8_t key[32],const char *id,void *arg)
{ (void)arg; assert(!strcmp(id,"0011223344556677")); memcpy(peer_key,key,32); return 0; }
static int load(uint8_t key[32],const char *id,void *arg)
{ (void)arg; if(strcmp(id,"0011223344556677")) return -1; memcpy(key,peer_key,32); return 0; }
static void alter(uint8_t *message,size_t length,uint8_t type)
{
    for(size_t i=0;i+2<=length;) {
        size_t size=message[i+1]; assert(i+2+size<=length);
        if(message[i]==type && size) { message[i+2]^=1; return; }
        i+=2+size;
    }
    assert(!"missing mutation target");
}
int pairing_crypto_vectors(void);
static void verify_rejection(const char *saved,int wrong_key)
{
    char keys[193]; memcpy(keys,saved,sizeof(keys)); if(wrong_key) keys[191]=keys[191]=='0' ? '1' : '0';
    struct pair_verify_context *client=pair_verify_new(PAIR_CLIENT_HOMEKIT_NORMAL,keys,NULL,NULL,"0011223344556677");
    struct pair_verify_context *server=pair_verify_new(PAIR_SERVER_HOMEKIT,NULL,load,NULL,"AA:BB:CC:DD:EE:FF"); assert(client && server);
    uint8_t *request,*response=NULL; size_t request_length,response_length;
    request=pair_verify_request1(&request_length,client); assert(request);
    assert(!pair_verify(&response,&response_length,server,request,request_length)); free(request);
    if(!wrong_key) alter(response,response_length,TLVType_EncryptedData);
    emit(wrong_key ? "verify-wrong-stored-key-response" : "verify-tampered-envelope-response",response,response_length);
    assert(pair_verify_response1(client,response,response_length)<0);
    struct pair_result *secret; assert(pair_verify_result(&secret,client)<0);
    puts(wrong_key ? "verify-stored-key rejected" : "verify-authenticated-envelope rejected");
    free(response); pair_verify_free(client); pair_verify_free(server);
}
static void run(int transient,int wrong_pin,int tamper)
{
    counter=0; memset(peer_key,0,sizeof(peer_key));
    enum pair_type type=transient ? PAIR_CLIENT_HOMEKIT_TRANSIENT : PAIR_CLIENT_HOMEKIT_NORMAL;
    struct pair_setup_context *client=pair_setup_new(type,transient ? NULL : wrong_pin ? "111-22-333" : "314-15-926",NULL,NULL,"0011223344556677");
    struct pair_setup_context *server=pair_setup_new(PAIR_SERVER_HOMEKIT,transient ? "3939" : "314-15-926",save,NULL,"AA:BB:CC:DD:EE:FF");
    assert(client && server);
    printf("case %s\n",transient ? "transient" : wrong_pin ? "wrong-pin" : tamper ? "tampered-proof" : "normal");
    uint8_t *request,*response=NULL; size_t request_length,response_length=0;
    request=pair_setup_request1(&request_length,client); assert(request); emit("setup-client-start",request,request_length);
    assert(!pair_setup(&response,&response_length,server,request,request_length)); free(request);
    emit("setup-server-challenge",response,response_length); assert(!pair_setup_response1(client,response,response_length)); free(response); response=NULL;
    request=pair_setup_request2(&request_length,client); assert(request);
    if(tamper) alter(request,request_length,TLVType_Proof);
    emit("setup-client-proof",request,request_length);
    int result=pair_setup(&response,&response_length,server,request,request_length); free(request);
    if(wrong_pin || tamper) {
        assert(!result && response); emit("setup-authentication-error",response,response_length);
        assert(pair_setup_response2(client,response,response_length)<0);
        struct pair_result *secret; assert(pair_setup_result(NULL,&secret,client)<0);
        puts("authentication rejected"); goto done;
    }
    assert(!result); emit("setup-server-proof",response,response_length); assert(!pair_setup_response2(client,response,response_length)); free(response); response=NULL;
    if(!transient) {
        request=pair_setup_request3(&request_length,client); assert(request); emit("setup-client-identity",request,request_length);
        assert(!pair_setup(&response,&response_length,server,request,request_length)); free(request);
        emit("setup-server-identity",response,response_length); assert(!pair_setup_response3(client,response,response_length)); free(response); response=NULL;
    }
    struct pair_result *secret; const char *saved=NULL;
    assert(!pair_setup_result(&saved,&secret,client)); emit("setup-secret",secret->shared_secret,secret->shared_secret_len);
    if(transient) { assert(secret->shared_secret_len==64); goto done; }
    assert(saved && strlen(saved)==192); emit("saved-public-test-credentials",saved,strlen(saved));
    struct pair_verify_context *verify=pair_verify_new(type,saved,NULL,NULL,"0011223344556677");
    struct pair_verify_context *peer=pair_verify_new(PAIR_SERVER_HOMEKIT,NULL,load,NULL,"AA:BB:CC:DD:EE:FF"); assert(verify && peer);
    request=pair_verify_request1(&request_length,verify); assert(request); emit("verify-client-key",request,request_length);
    assert(!pair_verify(&response,&response_length,peer,request,request_length)); free(request);
    emit("verify-server-key-signature",response,response_length); assert(!pair_verify_response1(verify,response,response_length)); free(response); response=NULL;
    request=pair_verify_request2(&request_length,verify); assert(request); emit("verify-client-signature",request,request_length);
    assert(!pair_verify(&response,&response_length,peer,request,request_length)); free(request);
    emit("verify-server-complete",response,response_length); assert(!pair_verify_response2(verify,response,response_length)); free(response); response=NULL;
    struct pair_result *remote; assert(!pair_verify_result(&secret,verify) && !pair_verify_result(&remote,peer));
    assert(secret->shared_secret_len==32 && remote->shared_secret_len==32 && !memcmp(secret->shared_secret,remote->shared_secret,32)); emit("verify-secret",secret->shared_secret,32);
    struct pair_cipher_context *encrypt=pair_cipher_new(type,PAIR_CHANNEL_CONTROL,secret->shared_secret,32,NULL);
    struct pair_cipher_context *decrypt=pair_cipher_new(PAIR_SERVER_HOMEKIT,PAIR_CHANNEL_CONTROL,remote->shared_secret,32,NULL); assert(encrypt && decrypt);
    const uint8_t plain[]="SETUP rtsp://127.0.0.1/session RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    assert(pair_encrypt(&request,&request_length,plain,sizeof(plain)-1,encrypt)==(ssize_t)sizeof(plain)-1); emit("control-cipher",request,request_length);
    assert(pair_decrypt(&response,&response_length,request,request_length,decrypt)==(ssize_t)request_length && response_length==sizeof(plain)-1 && !memcmp(response,plain,response_length)); free(response); response=NULL;
    request[request_length-1]^=1; pair_decrypt_rollback(decrypt);
    assert(pair_decrypt(&response,&response_length,request,request_length,decrypt)<0);
    /* The frozen implementation releases the failed output internally. */
    response=NULL; puts("control-tag rejected"); free(request);
    pair_cipher_free(encrypt); pair_cipher_free(decrypt); pair_verify_free(verify); pair_verify_free(peer);
    verify_rejection(saved,0); verify_rejection(saved,1);
done:
    free(response); pair_setup_free(client); pair_setup_free(server);
}
int main(void)
{
    randombytes_implementation implementation={random_name,random_word,NULL,NULL,random_fill,NULL};
    assert(!randombytes_set_implementation(&implementation) && sodium_init()>=0);
    assert(gcry_check_version(NULL)); assert(!gcry_control(GCRYCTL_DISABLE_SECMEM,0)); assert(!gcry_control(GCRYCTL_INITIALIZATION_FINISHED,0));
    assert(!pairing_crypto_vectors()); run(0,0,0); run(1,0,0); run(0,1,0); run(0,0,1);
    struct pair_setup_context *setup=pair_setup_new(PAIR_CLIENT_HOMEKIT_NORMAL,"314-15-926",NULL,NULL,"0011223344556677"); assert(setup);
    const uint8_t invalid_state[]={6,1,9}; uint8_t *output=NULL; size_t length=0;
    assert(pair_setup(&output,&length,setup,invalid_state,sizeof(invalid_state))<0 && !output && !length);
    pair_setup_free(setup); puts("unsupported-state rejected");
    return 0;
}
