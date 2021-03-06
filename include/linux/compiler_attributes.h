#ifndef _COMPAT_COMPILER_ATRR_H
#define _COMPAT_COMPILER_ATTR_H

#include_next <linux/compiler_attributes.h>
#include <linux/types.h>
#include "../../compat/config.h"

#ifndef __GCC4_has_attribute___fallthrough__
# define __GCC4_has_attribute___fallthrough__         0
 /* Add the pseudo keyword 'fallthrough' so case statement blocks
 * must end with any of these keywords:
 *   break;
 *   fallthrough;
 *   goto <label>;
 *   return [expression];
 *
 *  gcc: https://gcc.gnu.org/onlinedocs/gcc/Statement-Attributes.html#Statement-Attributes
 */
#undef fallthrough
#if __has_attribute(__fallthrough__)
# define fallthrough                    __attribute__((__fallthrough__))
#else
# define fallthrough                    do {} while (0)  /* fallthrough */
#endif
#endif /* __GCC4_has_attribute___fallthrough__ */


#endif /* _COMPAT_COMPILER_ATTR_H */
