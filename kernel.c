#include "defs.h"

void kernel_init() {
  // We cannot use VMCOREINFO_OFFSET to get 'name'
  // from struct uts_namespace. Fortunately, 'release' has already
  // been recorded in VMCOREINFO_OSRELEASE, so let's simply search
  // it from vmcoreinfo_data.

  parse_kernel_version(vmcoreinfo_read_string("OSRELEASE"));
}
