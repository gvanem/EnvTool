#ifndef _DIRLIST_H
#define _DIRLIST_H

#include <windows.h>
#include <sys/types.h>

#if !defined(__CYGWIN__)
#include <direct.h>
#endif

enum od2x_sorting {
     OD2X_UNSORTED,
     OD2X_ON_NAME,
     OD2X_FILES_FIRST,
     OD2X_DIRECTORIES_FIRST
   };

struct od2x_options {
       const char       *pattern;
       enum od2x_sorting sort;
       int               recursive;
     };

struct dirent2 {
       ino_t     d_ino;          /* a bit of a farce */
       size_t    d_reclen;       /* more farce */
       size_t    d_namlen;       /* length of d_name */
       char     *d_name;         /* MALLOC()'ed fully qualified file-name */
       DWORD     d_attrib;       /* FILE_ATTRIBUTE_xx. Ref MSDN. */
       FILETIME  d_time_create;
       FILETIME  d_time_access;  /* always midnight local time */
       FILETIME  d_time_write;
       DWORD64   d_fsize;
     };

typedef struct _dirdesc2 {
        size_t          dd_loc;       /* index into below dd_contents[] */
        size_t          dd_num;       /* max # of entries in dd_contents[] */
        struct dirent2 *dd_contents;  /* pointer to contents of dir */
      } DIR2;

extern DIR2           *opendir2 (const char *dir);
extern DIR2           *opendir2x (const char *dir, struct od2x_options *_opts);
extern struct dirent2 *readdir2 (DIR2 *dp);
extern void            seekdir2 (DIR2 *dp, long ofs);
extern long            telldir2 (DIR2 *dp);
extern void            rewinddir2 (DIR2 *dp);
extern void            closedir2 (DIR2 *dp);

/*
 * Comparison routines for scandir2() and opendir2x():
 *   If 'od2x_options::sort == OD2X_ON_NAME'           -> use 'sd_compare_alphasort()'.
 *   If 'od2x_options::sort == OD2X_FILES_FIRST'       -> use 'sd_compare_files_first()'.
 *   If 'od2x_options::sort == OD2X_DIRECTORIES_FIRST' -> use 'sd_compare_dirs_first()'.
 */
extern int sd_compare_alphasort   (const void **a, const void **b);
extern int sd_compare_files_first (const void **a, const void **b);
extern int sd_compare_dirs_first  (const void **a, const void **b);

/*
 * arg1 = directory name
 * arg2 = unallocated array of pointers to dirent(direct) strctures
 * arg3 = specifier for which objects to pick (pointer to function)
 * arg4 = sorting function pointer to pass to qsort
 *
 * Returns number of files added to namelist[].
 * Or -1 on error.
 */
extern int scandir2 (const char *dirname,
                     struct dirent2 ***namelist,
                     int (*sd_select)(const struct dirent2 *),
                     int (*dcomp)(const void **, const void **));

#endif /* _DIRLIST_H */
