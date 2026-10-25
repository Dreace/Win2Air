#include "sender.h"
#include <assert.h>
#include <string.h>
int main(void) {
    sender_abi_info actual,expected=sender_header_abi();
    assert(sender_get_abi(SENDER_ABI_VERSION,sizeof(actual),&actual)==SENDER_OK);
    assert(!memcmp(&actual,&expected,sizeof(actual)));
    assert(sender_get_abi(999,sizeof(actual),&actual)==SENDER_INPUT);
    assert(sender_get_abi(SENDER_ABI_VERSION,1,&actual)==SENDER_INPUT);
    sender_create_options options={SENDER_ABI_VERSION,sizeof(options),{1,"ABI consumer","ABI consumer",4096,NULL,NULL,NULL},{1000,0},NULL};
    sender_runtime* runtime=NULL; sender_error error;
    assert(sender_create_ex(&options,&runtime,&error)==SENDER_OK);
    assert(sender_destroy(runtime,&error)==SENDER_OK); runtime=NULL;
    options.size=1;
    assert(sender_create_ex(&options,&runtime,&error)==SENDER_INPUT && !runtime);
    return 0;
}
