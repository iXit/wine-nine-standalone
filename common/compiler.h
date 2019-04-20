/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __COMPILER_H
#define __COMPILER_H

#define NINE_ATTR_PRINTF(index, check) __attribute__((format(printf, index, check)))
#define NINE_ATTR_ALIGNED(alignment) __attribute__((aligned(alignment)))

#endif /* __COMPILER_H */
