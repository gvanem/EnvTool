/** \file cmake.h
 *  \ingroup Misc
 */
#ifndef _CMAKE_H
#define _CMAKE_H

BOOL cmake_get_info (char **exe, struct ver_info *ver);
int  cmake_cache_info_registry (void);
int  cmake_get_info_registry (int *index, HKEY top_key);

#endif

