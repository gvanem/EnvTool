/** \file auth.h
 * \ingroup Authentication
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

extern int  netrc_lookup (const char *host, const char **user, const char **passw);
extern int  authinfo_lookup (const char *host, const char **user, const char **passw, uint16_t *port);
extern int  envtool_cfg_lookup (const char *host, const char **user, const char **passw, uint16_t *port);

extern void netrc_exit (void);
extern void authinfo_exit (void);
extern void envtool_cfg_exit (void);
extern bool auth_envtool_handler (const char *section, const char *key, const char *value);

