/**\file    vcpkg.c
 * \ingroup Misc
 * \brief
 *   An interface for Microsoft's Package Manager VCPKG.
 *   https://github.com/Microsoft/vcpkg
 */
#include "envtool.h"
#include "smartlist.h"
#include "color.h"
#include "dirlist.h"
#include "vcpkg.h"

#define CONTROL_DESCRIPTION   "Description: "
#define CONTROL_SOURCE        "Source: "
#define CONTROL_VERSION       "Version: "
#define CONTROL_BUILD_DEPENDS "Build-Depends: "

static smartlist_t *vcpkg_nodes;

/* Save nodes releative to this directory to save memory.
 */
static char vcpkg_base_dir [_MAX_PATH];

/*
 * Local functions
 */
static char *get_parent (const char *full_path, char *fname)
{
  while (*fname && fname > full_path)
  {
    if (IS_SLASH(*fname))
       break;
    fname--;
  }
  return (fname+1);
}

static void CONTROL_parse (struct vcpkg_node *node, const char *file)
{
  FILE  *f = fopen (file, "r");
  char  *buf, *p;
  size_t buf_sz;

  if (!f)
     return;

  buf_sz = max (VCPKG_MAX_DESCR, 500);
  buf = alloca (buf_sz);

  while (1)
  {
    if (!fgets(buf,buf_sz-1,f))   /* EOF */
       break;

    strip_nl (buf);
    p = str_ltrim (buf);

    if (!node->descr && !strnicmp(p,CONTROL_DESCRIPTION,sizeof(CONTROL_DESCRIPTION)-1))
       node->descr = STRDUP (p + sizeof(CONTROL_DESCRIPTION) - 1);

    if (!node->build_depends && !strnicmp(p,CONTROL_BUILD_DEPENDS,sizeof(CONTROL_BUILD_DEPENDS)-1))
       node->build_depends = STRDUP (p + sizeof(CONTROL_BUILD_DEPENDS) - 1);

    else if (!node->source[0] && !strnicmp(p,CONTROL_SOURCE,sizeof(CONTROL_SOURCE)-1))
        _strlcpy (node->source, p + sizeof(CONTROL_SOURCE) - 1, sizeof(node->source));

    else if (!node->version[0] && !strnicmp(p,CONTROL_VERSION,sizeof(CONTROL_VERSION)-1))
       _strlcpy (node->version, p + sizeof(CONTROL_VERSION) - 1, sizeof(node->version));

    /* Quit when we've got all we need
     */
    if (node->descr && node->build_depends && node->source[0] && node->version[0])
       break;
  }
  fclose (f);
}

/*
 * Parse `file` for LOCAL package location or REMOTE package URL
 */
static void portfile_cmake_parse (struct vcpkg_node *node, const char *file)
{
}

static void vcpkg_build_list (const char *dir, const struct od2x_options *opts)
{
  struct vcpkg_node *node;
  struct dirent2     *de;
  DIR2               *dp = opendir2x (dir, opts);
  char               *this_file, *this_dir, *p;
  char                file [_MAX_PATH];

  if (!dp)
     return;

  while ((de = readdir2(dp)) != NULL)
  {
    node = NULL;

    if (de->d_attrib & FILE_ATTRIBUTE_DIRECTORY)
    {
      /* The recursion level should be limited since VCPKG seems to supprt "CONTROL"
       * and "portfile.cmake" files at one level below "%VCPKG_ROOT%\\ports" only.
       */
      vcpkg_build_list (de->d_name, opts);
    }
    else
    {
      this_file = basename (de->d_name);
      p = this_file-1;
      *p = '\0';
      this_dir = get_parent (de->d_name, p-1);

      if (!stricmp(this_file,"CONTROL"))
      {
        node = CALLOC (sizeof(*node), 1);
        node->have_CONTROL = TRUE;
        snprintf (file, sizeof(file), "%s\\%s\\%s", vcpkg_base_dir, this_dir, this_file);
        CONTROL_parse (node, file);
      }
      else  if (!stricmp(this_file,"portfile.cmake"))
      {
        node = CALLOC (sizeof(*node), 1);
        node->have_portfile_cmake = TRUE;
        snprintf (file, sizeof(file), "%s\\%s\\%s", vcpkg_base_dir, this_dir, this_file);
        portfile_cmake_parse (node, file);
      }
      if (node)
      {
        _strlcpy (node->package, this_dir, sizeof(node->package));
        smartlist_add (vcpkg_nodes, node);
      }
    }
  }
  closedir2 (dp);
}

/**
 * Build the smartlist `vcpkg_nodes`.
 */
int vcpkg_list (void)
{
  struct od2x_options opts;
  const char *env = getenv ("VCPKG_ROOT");

  if (!env)
  {
    WARN ("Env-var 'VCPKG_ROOT' not defined.\n");
    return (0);
  }

  vcpkg_nodes = smartlist_new();

  snprintf (vcpkg_base_dir, sizeof(vcpkg_base_dir), "%s\\ports", env);
  memset (&opts, '\0', sizeof(opts));
  opts.pattern = "*";

  vcpkg_build_list (vcpkg_base_dir, &opts);
  return smartlist_len (vcpkg_nodes);
}

/**
 * Free the memory allocated for `vcpkg_nodes`.
 */
void vcpkg_free (void)
{
  int i, max = vcpkg_nodes ? smartlist_len (vcpkg_nodes) : 0;

  for (i = 0; i < max; i++)
  {
    struct vcpkg_node *node = smartlist_get (vcpkg_nodes, i);

    FREE (node->descr);
    FREE (node->build_depends);
    FREE (node);
  }
  smartlist_free (vcpkg_nodes);
  vcpkg_nodes = NULL;
}

void vcpkg_debug (int level, int i)
{
  const struct vcpkg_node *node = smartlist_get (vcpkg_nodes, i);

  DEBUGF (level, "%4d: %d, %d, %s\\%s\n", i, node->have_CONTROL, node->have_portfile_cmake, vcpkg_base_dir, node->package);
}

int vcpkg_get_control (const struct vcpkg_node **node_p, const char *packages)
{
  const struct vcpkg_node *node;
  static int i, max;

  if (*node_p == NULL)
  {
    max = vcpkg_nodes ? smartlist_len (vcpkg_nodes) : 0;
    i = 0;
  }

  while (i < max)
  {
    vcpkg_debug (1, i);
    node = smartlist_get (vcpkg_nodes, i++);
    if (!node->have_CONTROL)
       continue;

    if (fnmatch(packages, node->package, FNM_FLAG_NOCASE) == FNM_MATCH)
    {
      *node_p = node;
      break;
     }
  }
  return (i < max ? 1 : 0);
}

int vcpkg_dump (void)
{
  int i, max = vcpkg_nodes ? smartlist_len (vcpkg_nodes) : 0;

  for (i = 0; i < max; i++)
      vcpkg_debug (0, i);
  return (max);
}

int vcpkg_dump_control (const char *packages)
{
  const struct vcpkg_node *node;
  int   old, matches = 0;

  C_printf ("Dumping CONTROL for packages matching ~6%s~0.\n", packages);

  for (node = NULL; vcpkg_get_control(&node, packages); matches++)
  {
    C_printf ("~6%s~0: ", node ? node->package : "<none?!>");

    /* In case some other fields contains a `~'
     */
    old = C_setraw (1);

    if (node && node->descr)
         C_puts_long_line (node->descr, sizeof("  descr  : ")-1);
    else C_puts ("None\n");

    C_puts ("  dependants: ");
    if (node && node->build_depends)
         C_puts_long_line (node->build_depends, sizeof("  descr  : ")-1);
    else C_puts ("Nothing\n");

    C_printf ("  version:    %s\n", node ? node->version : "<none>");
    C_setraw (old);
  }
  return (matches);
}

