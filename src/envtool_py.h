/** \file envtool_py.h
 *  \ingroup Envtool_PY
 */
#ifndef _ENVTOOL_PY_H
#define _ENVTOOL_PY_H

/**
 * \typedef enum python_variants
 * The types of Python we support.
 */
typedef enum python_variants {
        UNKNOWN_PYTHON,      /**< we have not found any suitable Python yet. */
        DEFAULT_PYTHON,      /**< a suitable Python found first on `PATH`. */
        PY2_PYTHON,          /**< a version 2.x Python was found and selected. */
        PY3_PYTHON,          /**< a version 3.x Python was found and selected. */
        IRON2_PYTHON,        /**< a version 2.x IronPython was found and selected. */
        IRON3_PYTHON,        /**< a version 3.x IronPython was found and selected. */
        PYPY_PYTHON,         /**< a PyPy was found and selected (any version). */
        JYTHON_PYTHON,       /**< a JavaPython was found and selected (any version). */
        ALL_PYTHONS          /**< any Python found. */
      } python_variants;

extern enum python_variants py_which;

struct python_info; /* Forward */

extern void         py_init          (void);
extern void         py_exit          (void);
extern int          py_search        (void);
extern void         py_searchpaths   (void);
extern int          py_test          (void);
extern int          py_get_info      (char **exe, char **dll, struct ver_info *ver);
extern const char **py_get_variants  (void);
extern const char  *py_variant_name  (enum python_variants v);
extern int          py_variant_value (const char *short_name, const char *full_name);
extern int          py_print_modules (void);
struct python_info *py_select        (enum python_variants which);
extern char        *py_execfile      (const char **py_argv, BOOL capture);

#endif  /* _ENVTOOL_PY_H */

