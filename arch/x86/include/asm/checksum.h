/* SPDX-License-Identifier: GPL-2.0 */
#ifdef CONFIG_GENERIC_CSUM
# include <asm-generic/checksum.h>
#else
# ifdef CONFIG_X86_32
#  include <asm/checksum_32.h>
# else
#  include <asm/checksum_64.h>
# endif
#endif
