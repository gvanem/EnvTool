/** \file ignore.h
 */
#ifndef _IGNORE_H
#define _IGNORE_H

extern int  cfg_ignore_init (const char *fname);
extern void cfg_ignore_exit (void);
extern int  cfg_ignore_lookup (const char *section, const char *value);

#endif /* _IGNORE_H */