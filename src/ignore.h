/** \file ignore.h
 *  \ingroup Misc
 */
#pragma once

#include <stdbool.h>

extern void        cfg_ignore_exit (void);
extern int         cfg_ignore_lookup (const char *section, const char *value);
extern const char *cfg_ignore_first (const char *section);
extern const char *cfg_ignore_next (const char *section);
extern void        cfg_ignore_dump (void);
extern bool        cfg_ignore_handler (const char *section,
                                       const char *key,
                                       const char *value);
