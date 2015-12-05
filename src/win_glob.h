/* Copyright (C) 2003 DJ Delorie, see COPYING.DJ for details */
/* Copyright (C) 2000 DJ Delorie, see COPYING.DJ for details */
/* Copyright (C) 1995 DJ Delorie, see COPYING.DJ for details */

#ifndef __dj_include_glob_h_
#define __dj_include_glob_h_

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

#define GLOB_MARK      0x01
#define GLOB_NOSORT    0x02
#define GLOB_RECURSIVE 0x04
#define GLOB_USE_EX    0x08

#define GLOB_NOMATCH  1
#define GLOB_NOSPACE  2

typedef struct {
        size_t  gl_pathc;
        char  **gl_pathv;
        size_t  gl_offs;
      } glob_t;

int glob (const char *_pattern, int _flags,
          int (*_errfunc)(const char *_epath, int _eerrno),
          glob_t *_pglob);

void globfree (glob_t *_pglob);

/***********************************************************************************/

struct glob_new_entry;

typedef struct {
        size_t                 gl_pathc;
        struct glob_new_entry *gl_pathv;
      } glob_new_t;

int glob_new (const char *_dir, int _flags,
              int (*callback)(const char *path),
              glob_new_t *_pglob);

void globfree_new (glob_new_t *_pglob);

#ifdef __cplusplus
}
#endif

#endif /* !__dj_include_glob_h_ */
