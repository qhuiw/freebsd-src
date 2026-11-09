/* Keep string loads within MTE allocation-tag granules. */
#define	__strrchr_aarch64_mte	strrchr
#include "aarch64/strrchr-mte.S"

.weak rindex
.equ rindex, strrchr
