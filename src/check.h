/** \file check.h
 */
#pragma once

#include <stdbool.h>

int  check_handler (void);
void check_exit (void);
bool check_cfg_handler (const char *section, const char *key, const char *value);

