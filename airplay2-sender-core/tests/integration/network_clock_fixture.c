#include "platform.h"
#include "ptpd.h"
#include <assert.h>
#include <string.h>

/* Network acceptance has a controlled clock identity and no PTP service.
 * This fixture is linked only into tests, never into the sender library. */
uint64_t ptpd_clock_id_get(void) { return UINT64_C(0x57494e32434f5245); }
int ptpd_find_or_bind(void) { return 0; }
int ptpd_init(uint64_t identity) { assert(identity); return 0; }
int ptpd_slave_add(uint32_t *id, const char *address)
{
    assert(id && address && *address);
    *id = 1;
    return 0;
}
void ptpd_slave_remove(uint32_t id) { assert(id == 1); }
void ptpd_deinit(void) {}
