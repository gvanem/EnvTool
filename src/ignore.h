/** \file ignore.h
 *  \ingroup Misc
 */
#ifndef _IGNORE_H
#define _IGNORE_H

extern int         cfg_ignore_init (const char *fname);
extern void        cfg_ignore_exit (void);
extern int         cfg_ignore_lookup (const char *section, const char *value);
extern const char *cfg_ignore_first (const char *section);
extern const char *cfg_ignore_next (const char *section);
extern void        cfg_ignore_dump (void);
extern void        cfg_ignore_parser (const char *section,
                                      const char *key,
                                      const char *value,
                                      unsigned    line);

#endif /* _IGNORE_H */
