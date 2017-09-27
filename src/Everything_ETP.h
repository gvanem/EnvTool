#ifndef _EVERYTHING_ETP_H_
#define _EVERYTHING_ETP_H_

extern DWORD ETP_total_rcv;

extern int  do_check_evry_ept (const char *host);
extern int  netrc_init (void);
extern void netrc_exit (void);
extern int  netrc_lookup (const char *host, const char **user, const char **passw);

extern int  authinfo_init (void);
extern void authinfo_exit (void);
extern int  authinfo_lookup (const char *host, const char **user, const char **passw, int *port);

#endif
