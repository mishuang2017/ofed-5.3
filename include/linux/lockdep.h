#ifndef COMPAT_LOCKDEP_H
#define COMPAT_LOCKDEP_H

#include_next <linux/lockdep.h>

/* Include the autogenerated header file */
#include "../../compat/config.h"

#ifndef lockdep_assert_held_write
#define lockdep_assert_held_write(l)    do {                    \
	WARN_ON(debug_locks && !lockdep_is_held_type(l, 0));    \
} while (0)
#endif

#endif /* COMPAT_LOCKDEP_H */
