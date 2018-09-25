/** \file vcpkg.h
 */
#ifndef _VCPKG_H
#define _VCPKG_H

/**
 * The size of some VCPKG entries.
 */
#define VCPKG_MAX_NAME     30
#define VCPKG_MAX_VERSION  30
#define VCPKG_MAX_DEPENDS  60   /* Boost-* packages can have MANY dependencies */

/**
 * \enum VCPKG_platform
 * The platform enumeration.
 */
typedef enum VCPKG_platform {
        VCPKG_plat_ALL     = 0,
        VCPKG_plat_WINDOWS = 0x0001,
        VCPKG_plat_UWP     = 0x0002,
        VCPKG_plat_LINUX   = 0x0004,
        VCPKG_plat_x64     = 0x0008,
        VCPKG_plat_ARM     = 0x0010,
        VCPKG_plat_ANDROID = 0x0020,
        VCPKG_plat_OSX     = 0x0040
      } VCPKG_platform;

struct vcpkg_depend {
       char  package [VCPKG_MAX_NAME];  /**< The package name */
       VCPKG_platform platform;         /**< The OS platform for this package */
     };

/**
 * \struct vcpkg_node
 * The structure of a single VCPKG package entry in the `vcpkg_nodes`.
 */
struct vcpkg_node {
       char  package [VCPKG_MAX_NAME];     /**< The packge name */
       char  version [VCPKG_MAX_VERSION];  /**< The version */
       char *description;                  /**< The description */
       BOOL  have_CONTROL;

       /**< The dependencies; a smartlist of `struct vcpkg_depend`
        */
       smartlist_t *deps;
     };

extern int         vcpkg_get_list (void);
extern void        vcpkg_free (void);
extern int         vcpkg_dump_control (const char *packages);
extern int         vcpkg_get_control (const struct vcpkg_node **node_p, const char *packages);
extern int         vcpkg_get_dep_platform (const struct vcpkg_depend *dep, BOOL *Not);
extern const char *vcpkg_get_dep_name (const struct vcpkg_depend *dep);
extern const char *vcpkg_last_error (void);

#endif



