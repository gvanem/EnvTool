/** \file    lua.h
 *  \ingroup Lua
 */
#ifndef _LUA_H
#define _LUA_H

extern void        lua_init (void);
extern void        lua_exit (void);
extern int         lua_search (const char *search_spec);
extern void        lua_check_env (const char *env, int *num, char *status, size_t status_sz);
extern BOOL        lua_get_info (char **exe, struct ver_info *ver);
extern const char *lua_get_exe (void);
extern BOOL        lua_cfg_handler (const char *section, const char *key, const char *value);

#endif
