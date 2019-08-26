/** \file auth.h
 * \ingroup Authentication
 */
#ifndef _AUTH_H_
#define _AUTH_H_

extern int  netrc_lookup (const char *host, const char **user, const char **passw);
extern int  authinfo_lookup (const char *host, const char **user, const char **passw, int *port);
extern int  envtool_cfg_lookup (const char *host, const char **user, const char **passw, int *port);

extern void netrc_exit (void);
extern void authinfo_exit (void);
extern void envtool_cfg_exit (void);
extern void auth_envtool_handler (const char *section, const char *key, const char *value);

#endif
