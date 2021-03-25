/**
 * \file    win_sqlite3.c
 * \ingroup Misc
 * \brief   Function for read/write to envtool Sqlite3 cache
 *
 * Currently called only from 'tests.c' (calls 'test_sqlite3()').
 * It is meant to replace or enhance functions in 'cache.c' later.
 */
#include "envtool.h"
#include "color.h"

#if defined(USE_SQLITE3)   /* rest of file */

#define SQLITE_API       __declspec(dllimport)
#define SQLITE_APICALL   __stdcall
#define SQLITE_CALLBACK  __stdcall

#if defined(HAVE_WINSQLITE3_H)
   #if defined(_MSC_VER)
    #include <winsqlite/winsqlite3.h>
  #else
    #include <winsqlite3.h>
  #endif
  #undef sqlite3_open
#else
  typedef struct sqlite3     sqlite3;
  typedef struct sqlite3_vfs sqlite3_vfs;
  typedef int (__stdcall*    sqlite3_callback) (void *cb_arg, int argc, char **argv, char **col_name);
#endif

#ifndef SQLITE_OPEN_READWRITE
#define SQLITE_OPEN_READWRITE 2
#endif

#ifndef SQLITE_OPEN_CREATE
#define SQLITE_OPEN_CREATE 4
#endif

#ifndef SQLITE_OK
#define SQLITE_OK 0
#endif

#define SQLITE_DLL_NAME  "WinSqlite3.dll"

static const char *db_name = "test.db";

static const char *exec_stmt[] = {
                  "CREATE TABLE IF NOT EXISTS tbl (column INTEGER, message TEXT, value INTEGER);",
                  "INSERT OR REPLACE INTO     tbl VALUES (1, 'hello!',         10);",
                  "INSERT OR REPLACE INTO     tbl VALUES (2, 'another string', 20);",
                  "INSERT OR REPLACE INTO     tbl VALUES (3, 'goodbye',        30);",
                  "SELECT *       FROM tbl WHERE column >= 2;",
               // "SELECT message FROM tbl WHERE message = 'hello!';"
                };

static HANDLE dll_hnd = NULL;

/*
 * Similar to 'envtool_py.c'
 */
#define DEF_FUNC(ret, f, args)                       \
        typedef ret (SQLITE_APICALL *func_##f) args; \
        static func_##f p_##f = NULL

#define LOAD_FUNC(is_opt, f)                               \
        do {                                               \
          p_##f = (func_##f) GetProcAddress (dll_hnd, #f); \
          if (!p_##f && !is_opt)                           \
          {                                                \
            WARN ("  Failed to find '%s()' in %s.\n",      \
                  #f, SQLITE_DLL_NAME);                    \
            return (FALSE);                                \
          }                                                \
          TRACE (2, "Function %s(): %*s 0x%p.\n",          \
                 #f, 23-(int)strlen(#f), "", p_##f);       \
        } while (0)

DEF_FUNC (int,          sqlite3_open, (const char *filename, sqlite3 **p_db));
DEF_FUNC (int,          sqlite3_open_v2, (const char *filename, sqlite3 **p_db,
                                          int flags, const char *vfs));

DEF_FUNC (int,          sqlite3_free, (void *data));
DEF_FUNC (int,          sqlite3_close, (sqlite3 *db));
DEF_FUNC (const char*,  sqlite3_errmsg, (sqlite3 *db));
DEF_FUNC (int,          sqlite3_extended_result_codes, (sqlite3*, int onoff));
DEF_FUNC (const char*,  sqlite3_libversion, (void));
DEF_FUNC (const char*,  sqlite3_sourceid, (void));
DEF_FUNC (sqlite3_vfs*, sqlite3_vfs_find, (const char *vfs_name));
DEF_FUNC (int,          sqlite3_exec, (sqlite3 *db, const char *statement,
                                       sqlite3_callback cb, void *cb_arg,
                                       char **p_err_msg));

static BOOL sql3_load (void)
{
  dll_hnd = LoadLibrary (SQLITE_DLL_NAME);
  if (!dll_hnd)
  {
    WARN ("  Failed to load %s; %s\n", SQLITE_DLL_NAME, win_strerror(GetLastError()));
    return (FALSE);
  }
  LOAD_FUNC (0, sqlite3_open);
  LOAD_FUNC (1, sqlite3_open_v2);
  LOAD_FUNC (0, sqlite3_exec);
  LOAD_FUNC (0, sqlite3_free);
  LOAD_FUNC (0, sqlite3_close);
  LOAD_FUNC (0, sqlite3_errmsg);
  LOAD_FUNC (1, sqlite3_extended_result_codes);
  LOAD_FUNC (0, sqlite3_libversion);
  LOAD_FUNC (0, sqlite3_sourceid);
  LOAD_FUNC (0, sqlite3_vfs_find);
  return (TRUE);
}

static BOOL sql3_unload (void)
{
  if (dll_hnd)
     FreeLibrary (dll_hnd);
  dll_hnd = NULL;
  return (TRUE);
}

static int SQLITE_CALLBACK sql3_callback (void *cb_arg, int argc, char **argv, char **col_name)
{
  int i;

  C_printf ("  ~3%s(): exec-number: %d~0  (argc: %d)\n",
            __FUNCTION__, (int)cb_arg, argc);

  for (i = 0; i < argc; i++)
      C_printf ("  ~6%-20s         ~2%s.~0\n",
                col_name[i], argv[i] ? argv[i] : "NULL");

  C_putc ('\n');
  return (0);
}

static void test_sqlite3_vfs (void)
{
#if defined(HAVE_WINSQLITE3_H)
  const struct sqlite3_vfs *vfs;
  int   i;

  C_printf ("  ~3%s():~0\n    ", __FUNCTION__);

  vfs = (*p_sqlite3_vfs_find) (NULL);
  for (i = 0; vfs; i++, vfs = vfs->pNext)
      C_printf ("~6zName: '%s'~0, ", vfs->zName);

  if (i == 0)
     C_puts   ("No Virtual File Systems.");
  C_puts ("\n\n");
#endif
}

/*
 * Called from tests.c / 'do_tests()'.
 */
void test_sqlite3 (void)
{
  struct sqlite3 *db = NULL;
  char   *err_msg = NULL;
  int     i, flags, rc;

  C_printf ("\n~3%s():~0\n", __FUNCTION__);

  /* Load the needed functions from the SQlite3 API. I.e. "WinSqlite3.dll".
   */
  if (!sql3_load())
     return;

  C_printf ("~2  sqlite3_libversion(): ~6%s~0\n"
            "~2  sqlite3_sourceid():   ~6%s~0.\n",
            (*p_sqlite3_libversion)(), (*p_sqlite3_sourceid)());

  test_sqlite3_vfs();

  /* Create a handle to the SQlite3 API
   */
  if (p_sqlite3_open_v2)
  {
    flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE /* | SQLITE_OPEN_MEMORY */;
    rc = (*p_sqlite3_open_v2) (db_name, &db, flags, NULL);
  }
  else
    rc = (*p_sqlite3_open) (db_name, &db);

  if (db && p_sqlite3_extended_result_codes)
     (*p_sqlite3_extended_result_codes) (db, 1);

  if (rc != SQLITE_OK)
  {
    WARN ("  Can't open database: rc: %d, %s\n", rc, (*p_sqlite3_errmsg)(db));
    (*p_sqlite3_close) (db);
    return;
  }

  for (i = 0; i < DIM(exec_stmt); i++)
  {
    rc = (*p_sqlite3_exec) (db, exec_stmt[i],
                            sql3_callback, (void*)i, &err_msg);
    if (rc != SQLITE_OK)
    {
      C_printf (" ~6%d: ~5SQL error:~0 rc: %d, %s\n", i, rc, err_msg);
      (*p_sqlite3_free) (err_msg);
      break;
    }
  }

  if (rc == SQLITE_OK && i == DIM(exec_stmt))
     C_printf ("  Successfully created ~6%s~0 and executed all statements.\n", db_name);

  (*p_sqlite3_close) (db);
  sql3_unload();
}
#endif /* USE_SQLITE3 */

