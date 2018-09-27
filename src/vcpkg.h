/** \file vcpkg.h
 */
#ifndef _VCPKG_H
#define _VCPKG_H

/**
 * \def VCPKG_MAX_NAME
 * \def VCPKG_MAX_VERSION
 */
#define VCPKG_MAX_NAME     30   /**< The max size of a `vcpkg_node::package` and `vcpkg_depend::package` entry */
#define VCPKG_MAX_VERSION  30   /**< The max size of a `vcpkg_node::version` entry */

/**
 * \enum VCPKG_platform
 * The platform enumeration.
 */
typedef enum VCPKG_platform {
        VCPKG_plat_ALL     = 0,        /**< Package is for all supported OSes */
        VCPKG_plat_WINDOWS = 0x0001,   /**< Package is for Windows only (desktop, not UWP). */
        VCPKG_plat_UWP     = 0x0002,   /**< Package is for Universal Windows Platform only. */
        VCPKG_plat_LINUX   = 0x0004,   /**< Package is for Linux only. */
        VCPKG_plat_x64     = 0x0008,   /**< Package is for x64 processors only. */
        VCPKG_plat_ARM     = 0x0010,   /**< Package is for ARM processors only. */
        VCPKG_plat_ANDROID = 0x0020,   /**< Package is for Android only. */
        VCPKG_plat_OSX     = 0x0040    /**< Package is for Apple's OSX only. */
      } VCPKG_platform;

/**\def VCPKG_platform_INVERSE
 * Package is the inverse of the above specified bit.
 */
#define VCPKG_platform_INVERSE 0x8000

/**
 * \struct vcpkg_depend
 * The structure of a package-dependency.
 */
struct vcpkg_depend {
       char  package [VCPKG_MAX_NAME];  /**< The package name */
       VCPKG_platform platform;         /**< The supported (or not supported) OS platform */
     };

/**
 * \struct vcpkg_node
 * The structure of a single VCPKG package entry in the `vcpkg_nodes`.
 */
struct vcpkg_node {
       char  package [VCPKG_MAX_NAME];     /**< The package name */
       char  version [VCPKG_MAX_VERSION];  /**< The version */
       char *description;                  /**< The description */
       BOOL  have_CONTROL;                 /**< TRUE if this is a CONTROL-node */

       /** The dependencies; a smartlist of `struct vcpkg_depend`.
        */
       smartlist_t *deps;

       /** The features; a smartlist of `char*`.
        */
       smartlist_t *features;
     };

extern unsigned    vcpkg_get_list (void);
extern unsigned    vcpkg_get_num_CONTROLS (void);
extern unsigned    vcpkg_get_num_portfile (void);

extern void        vcpkg_free (void);
extern unsigned    vcpkg_dump_control (const char *package_spec);
extern BOOL        vcpkg_get_control (int *index, const struct vcpkg_node **node_p, const char *package_spec);
extern int         vcpkg_get_dep_platform (const struct vcpkg_depend *dep, BOOL *Not);
extern const char *vcpkg_get_dep_name (const struct vcpkg_depend *dep);
extern const char *vcpkg_last_error (void);

#endif



