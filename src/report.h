/** \file report.h
 *  \ingroup Misc
 */
#pragma once

typedef struct report {
        const char *file;
        time_t      mtime;
        UINT64      fsize;
        BOOL        is_dir;
        BOOL        is_junction;
        BOOL        is_cwd;
        HKEY        key;
        DWORD       last_err;
        const char *filler;
        const char *content;
        int       (*pre_action) (struct report *r);
        int       (*post_action) (struct report *r);
      } report;

extern DWORD num_version_ok;
extern DWORD num_verified;
extern DWORD num_evry_dups;
extern DWORD num_evry_ignored;

extern int   report_file (report *r);
extern int   report_file2 (report *r);
extern void  report_header_print (void);
extern void  report_header_set (const char *fmt, ...);
extern void  report_final (int found);

