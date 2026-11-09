/* Keep string loads within MTE allocation-tag granules. */
#define	__strchr_aarch64_mte	strchr
#include "aarch64/strchr-mte.S"

.weak index
.equ index, strchr
