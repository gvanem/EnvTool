/** \file cache.h
 *  \ingroup Misc
 */
#pragma once

/**
 * \typedef CacheSections
 *
 * The cache sections we handle.
 */
typedef enum CacheSections {
             SECTION_FIRST = 0,   /* Do not use this */
             SECTION_CMAKE,
             SECTION_COMPILER,

             /**
              * For `CLASSPATH`, `PATH, `INCLUDE`, `MANPATH`,
              * `LIB`, `PERLLIBDIR`, `PKG_CONFIG_PATH` and
              * `PYTHONPATH` environment variables.
              */
             SECTION_ENV_DIR,
             SECTION_LUA,
             SECTION_PKGCONFIG,
             SECTION_PYTHON,
             SECTION_VCPKG,
             SECTION_TEST,
             SECTION_LAST         /* Do not use this */
           } CacheSections;

extern void        cache_init   (void);
extern void        cache_exit   (void);
extern void        cache_test   (void);
extern bool        cache_config (const char *key, const char *value);
extern void        cache_put    (CacheSections section, const char *key, const char *value);
extern void        cache_putf   (CacheSections section, _Printf_format_string_ const char *fmt, ...) ATTR_PRINTF(2, 3);
extern const char *cache_get    (CacheSections section, const char *key);
extern int         cache_getf   (CacheSections section, const char *fmt, ...);
extern void        cache_del    (CacheSections section, const char *key);
extern void        cache_delf   (CacheSections section, _Printf_format_string_ const char *fmt, ...) ATTR_PRINTF(2, 3);

