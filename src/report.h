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
       char       *filler;
       int       (*pre_action) (struct report *r);
       int       (*post_action) (struct report *r);
     };

extern int report_file2 (struct report *r);

#endif
