/** \file description.h
 *  \ingroup Misc
 */
#pragma once

#include <stdbool.h>

extern bool        file_descr_init (void);
extern void        file_descr_exit (void);
extern const char *file_descr_get (const char *file_dir);

