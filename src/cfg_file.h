/** \file cfg_file.h
 *  \ingroup Misc
 */
#ifndef _CFG_FILE_H
#define _CFG_FILE_H

/**
 * \struct cfg_node
 */
struct cfg_node {
       char    *section;  /**<  */
       char    *key;      /**<  */
       char    *value;    /**<  */
     };

/**
 * \enum cfg_sections
 */
enum cfg_sections {
     CFG_NONE = 0,    /**< The 'Unknown' section */
     CFG_GLOBAL,
     CFG_REGISTRY,
     CFG_COMPILER,
     CFG_EVERYTHING,
     CFG_PYTHON,
     CFG_PE_RESOURCES,
     CFG_LOGIN,
     CFG_MAX_SECTIONS
   };

typedef int (*cfg_parser) (const char *section,
                           const char *key,
                           const char *value,
                           unsigned    line);

extern int cfg_add_parser (enum cfg_sections section, cfg_parser parser);
extern int cfg_init (const char *fname);
extern int cfg_exit (void);

#endif

