/** \file cfg_file.h
 *  \ingroup Misc
 */
#pragma once

#include <stdbool.h>

/**
 * \typedef struct CFG_FILE
 * The opaque structure returned by `cfg_init()`.
 */
typedef struct CFG_FILE CFG_FILE;

/**
 * \typedef cfg_handler
 * A config-file handler should match this prototype.
 */
typedef bool (*cfg_handler) (const char *section,
                             const char *key,
                             const char *value);

extern CFG_FILE *cfg_init (const char *fname,
                           const char *section,
                        /* cfg_handler parser */ ...);

extern void cfg_exit (CFG_FILE *cf);
