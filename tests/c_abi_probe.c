/* Compiled as C: proves the SDK + platform headers are valid C and the C vtable views match. */
#include "sq/sq.h"
#include "sq/platform.h"

int sq_c_abi_probe(void);
size_t sq_c_sizeof_module_info(void);
size_t sq_c_sizeof_class_info(void);
size_t sq_c_factory_vtbl_slots(void);
size_t sq_c_host_vtbl_slots(void);

int sq_c_abi_probe(void) {
  sq_guid g;
  char buf[SQ_GUID_STRING_SIZE];
  sq_guid iunknown = SQ_IID_IUNKNOWN_INIT;
  if (!sq_guid_parse("{70519AAF-D202-4A40-94C8-39AF01486455}", &g)) return 0;
  if (!sq_guid_equal(&g, &iunknown)) return 0;
  sq_guid_format(&g, buf);
  if (buf[0] != '{' || buf[37] != '}' || buf[38] != '\0') return 0;
  if (sq_guid_parse("not-a-guid", &g)) return 0;
  if (!sq_abi_in_range(SQ_PLATFORM_ABI, SQ_ABI_DEFAULT_MIN, SQ_ABI_DEFAULT_MAX)) return 0;
  if (sq_crc32(0, "123456789", 9) != 0xCBF43926u) return 0;
  return 1;
}

size_t sq_c_sizeof_module_info(void) { return sizeof(sq_module_info); }
size_t sq_c_sizeof_class_info(void) { return sizeof(sq_class_info); }
size_t sq_c_factory_vtbl_slots(void) { return sizeof(sq_IClassFactoryVtbl) / sizeof(void*); }
size_t sq_c_host_vtbl_slots(void) { return sizeof(sq_IHostContextVtbl) / sizeof(void*); }
