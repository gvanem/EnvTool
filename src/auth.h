#ifndef _AUTH_H_
#define _AUTH_H_

extern int  netrc_init (void);
extern void netrc_exit (void);
extern int  netrc_lookup (const char *host, const char **user, const char **passw);

extern int  authinfo_init (void);
extern void authinfo_exit (void);
extern int  authinfo_lookup (const char *host, const char **user, const char **passw, int *port);

#endif
