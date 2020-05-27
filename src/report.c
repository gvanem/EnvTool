#include "envtool.h"
#include "color.h"
#include "report.h"

extern void print_raw (const char *file, const char *before, const char *after);

int report_file2 (struct report *r)
{
  FMT_buf     fmt_buf_file_info;
  FMT_buf     fmt_buf_time_size;
  const char *note = NULL;

  if (r->pre_action && !(*r->pre_action) (r))
     return (0);

  BUF_INIT (&fmt_buf_file_info, 100 + _MAX_PATH, 0);
  BUF_INIT (&fmt_buf_time_size, 100, 0);

  buf_printf (&fmt_buf_time_size, "~2%s~3%s%s: ",
              note ? note : r->filler, get_time_str(r->mtime), get_file_size_str(r->fsize));

  buf_printf (&fmt_buf_file_info, "~6%s%c", r->file, r->is_dir ? DIR_SEP: '\0');

  C_puts (fmt_buf_time_size.buffer_start);
  C_puts (fmt_buf_file_info.buffer_start);

  if (r->post_action)
    (*r->post_action) (r);

  C_puts ("~0\n");
  return (1);
}
