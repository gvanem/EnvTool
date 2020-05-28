/** \file report.h
 *  \ingroup Misc
 */
#ifndef _REPORT_H
#define _REPORT_H

struct report {
       const char *file;
       time_t      mtime;
       UINT64      fsize;
       BOOL        is_dir;
       BOOL        is_junction;
       HKEY        key;
       const char *filler;
       int       (*pre_action) (struct report *r);
       int       (*post_action) (struct report *r);
     };

extern DWORD num_version_ok;
extern DWORD num_verified;
extern DWORD num_evry_dups;
extern DWORD num_evry_ignored;

extern int  report_file (struct report *r);
extern int  report_file2 (struct report *r);
extern void report_header_print (void);
extern void report_header_set (const char *fmt, ...);
extern void report_final (int found);

#endif
