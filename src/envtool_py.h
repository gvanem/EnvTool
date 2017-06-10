#ifndef _ENVTOOL_PY_H
#define _ENVTOOL_PY_H

enum python_variants {
     UNKNOWN_PYTHON,
     DEFAULT_PYTHON,
     PY2_PYTHON,
     PY3_PYTHON,
     IRON2_PYTHON,
     IRON3_PYTHON,
     PYPY_PYTHON,
     JYTHON_PYTHON,
     ALL_PYTHONS,
   };

extern enum python_variants py_which;

struct python_info;

extern void         py_init          (void);
extern void         py_exit          (void);
extern int          py_search        (void);
extern void         py_searchpaths   (void);
extern int          py_test          (void);
extern int          py_get_info      (const char **exe, const char **dll, struct ver_info *ver);
extern const char **py_get_variants  (void);
extern const char  *py_variant_name  (enum python_variants v);
extern int          py_variant_value (const char *short_name, const char *full_name);
struct python_info *py_select        (enum python_variants which);

#endif  /* _ENVTOOL_PY_H */

