/** \file cmake.h
 *  \ingroup Misc
 */
#pragma once

/**
 * \def KITWARE_REG_NAME
 * The Kitware (Cmake) Registry key name under HKCU or HKLM.
 */
#define KITWARE_REG_NAME "Software\\Kitware\\CMake\\Packages"

bool cmake_get_info (char **exe, struct ver_info *ver);
int  cmake_get_info_registry (smartlist_t *sl, int *index, HKEY top_key);
int  cmake_search (void);

