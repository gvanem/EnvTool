EnvTool v1.0:
==============

A tool to search along various environment variables for files (or a wildcard). The following option-modes handles
these environment variables:

| `--path`      |  `%PATH%`     |
| ------------- | ------------- |
| `--inc`       | `%INCLUDE%`, `%C_INCLUDE_PATH%` and `%CPLUS_INCLUDE_PATH%` |
| `--lib`       | `%LIB%` and `%LIBRARY_PATH%`    |
| `--cmake`     | `%CMAKE_MODULE_PATH%`           | 
| `--man`       | `%MANPATH%`                     |
| `--pkg`       | `%PKG_CONFIG_PATH%`             |
| `--python`    | `%PYTHONPATH%` and `sys.path[]` |
| `--evry`      | [EveryThing](http://www.voidtools.com/support/everything) file database.|
| `--evry=host` | Remote search in a EveryThing database on `host` |

It also checks for missing directories along the above env-variables.

The `--path` option also checks these registry keys:
  `HKEY_CURRENT_USER\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths` and
  `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths`

and enumerates all keys for possible programs. E.g. if registry contains this:
  `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\makensis.exe` =
   `f:\MinGW32\bin\MingW-studio\makensis.exe`,

`envtool --path maken*` will include `f:\MinGW32\bin\MingW-studio\makensis.exe`
in the result.

Problem with old programs pestering your `PATH` and *Registry* entries can be tricky
to diagnose. Here I had an problem with an old version of the *FoxitReader PDF reader*:
Checking with `envtool --path foxit*.exe`, resulted in:
```
  Matches in HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths:
   (2)  27 Nov 2014 - 10:24:04: f:\ProgramFiler\FoxitReader\FoxitReader.exe
  Matches in %PATH:
        21 Apr 2006 - 17:43:10: f:\util\FoxitReader.exe
   (2): found in "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths".
```

Hence if you write `FoxitReader` in the Window Run-box (*Winkey-R*), you'll get the
newer (27 Nov 2014) `FoxitReader` launched. But if you say `FoxitReader` in your shell
(cmd etc.), you'll get the old version (21 Apr 2006).

Other examples:

**E.g. 1**: `envtool --path notepad*.exe` first checks the `%PATH%` env-var
 for consistency (reports missing directories in `%PATH%`) and prints
 all the locations of `notepad*.exe`. On my box the result is:
```
Thu Jul 21 16:02:20 2011 : f:\windows\system32\notepad-orig.exe
Mon Nov 18 19:26:40 2002 : f:\windows\system32\notepad.exe
Thu Jul 21 16:13:11 2011 : f:\windows\system32\notepad2.exe
Mon Nov 18 19:26:40 2002 : f:\windows\notepad.exe
```

**E.g. 2**: `envtool --inc afxwin*` first checks the `%INCLUDE%` env-var
for consistency (reports missing directories in `%INCLUDE`) and prints
all the locations of `afxwin*`. On my box the result is:
```
Thu Apr 14 18:54:46 2005 : g:\vc_2010\VC\AtlMfc\include\AFXWIN.H
Thu Apr 14 18:54:46 2005 : g:\vc_2010\VC\AtlMfc\include\AFXWIN1.INL
Thu Apr 14 18:54:46 2005 : g:\vc_2010\VC\AtlMfc\include\AFXWIN2.INL
Thu Apr 14 18:54:46 2005 : g:\vc_2010\VC\AtlMfc\include\AFXWIN.H
Thu Apr 14 18:54:46 2005 : g:\vc_2010\VC\AtlMfc\include\AFXWIN1.INL
Thu Apr 14 18:54:46 2005 : g:\vc_2010\VC\AtlMfc\include\AFXWIN2.INL
```

**E.g. 3**: If an *App Paths* registry key has an alias for a command, the target
program is printed. E.g. if:
`HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\winzip.exe`
points to `c:\PROGRA~1\WINZIP\winzip32.exe`

(here `winzip.exe` is an alias for the real program `winzip32.exe`). Hence
`envtool --path winzip*` reports:
```
Fri Oct 11 09:10:00 2002: f:\PROGRA~1\WINZIP\winzip.exe !
Fri Oct 11 09:10:00 2002: f:\PROGRA~1\WINZIP\winzip32.exe !
(!) - found in registry.
```

**E.g. 4**: It's pretty amazing what the *FindFile()* API in Windows can do. E.g.:
`envtool --path *-?++.exe`:
```
Tue Nov 19 12:01:38 2002 : f:\Mingw32\bin\mingw32-c++.exe
Tue Nov 19 12:01:38 2002 : f:\Mingw32\bin\mingw32-g++.exe
Wed Mar 09 14:39:05 2011 : f:\CygWin\bin\i686-pc-cygwin-c++.exe
Wed Mar 09 14:39:05 2011 : f:\CygWin\bin\i686-pc-cygwin-g++.exe
```

Although not as powerful as "POSIX-style file matching" which is also built-in
via the `fnmatch()` function.


**E.g. 5**: If you have Python installed, the `--python` option will search in
`%PYTHONPATH` and `sys.path[]` for a match. E.g.:
`envtool.exe --python ss*.py`:
```
24 Jun 2011 - 11:38:10: f:\ProgramFiler\Python27\lib\ssl.py
16 Feb 2011 - 12:14:28: f:\ProgramFiler\Python27\lib\site-packages\win32\lib\sspi.py
16 Feb 2011 - 12:14:28: f:\ProgramFiler\Python27\lib\site-packages\win32\lib\sspicon.py
```

**E.g. 6**: The `--python` option wil also look inside Python *EGG*s (plain ZIP-files) found
in `sys.path[]`. E.g.:
`envtool.exe --python socket.py`:
```
27 Mar 2013 - 16:41:58: stem\socket.py  (%PYTHONHOME\lib\site-packages\stem-1.0.1-py2.7.egg)
30 Apr 2014 - 09:54:04: f:\Programfiler\Python27\lib\socket.py
```

**E.g. 7**: The `--evry` option combined with the `--regex` (or `-r`) is quite powerful. To find
all directories with Unix man-pages, you can do this:
`envtool.exe --evry -r "man[1-9]$"`:
```
<DIR> 03 Jun 2014 - 17:50:36: f:\CygWin\lib\perl5\5.14\Parse-Yapp-1.05\blib\man1\
<DIR> 03 Jun 2014 - 17:54:06: f:\CygWin\usr\man\man1\
<DIR> 03 Jun 2014 - 17:56:08: f:\CygWin\usr\share\man\man1\
<DIR> 03 Jun 2014 - 17:55:58: f:\CygWin\usr\share\man\bg\man1\
```

Which is probably a lot more directories than you have in you `%MANPATH%`.

Or to find only `foo*.bar` files under directory-branch(es) `misc`, you can do:
```
envtool.exe --evry -r "misc\\.*\\foo.*\.bar"
```

**E.g. 8**: More than one option-mode can combined. For example:
`envtool.exe --man --evry awk*.[1-9]*`:
```
Matches in %MANPATH:
      13 Mar 2011 - 19:47:43: f:\CygWin\usr\share\man\man1\awk.1.gz (f:\CygWin\usr\share\man\man1\gawk.1)
      09 Dec 1997 - 17:55:20: e:\djgpp\share\man\man1\awk.1
      15 Dec 2004 - 02:06:32: e:\djgpp\share\man\cat1\awk.1
Matches from EveryThing:
<DIR> 24 May 2014 - 12:48:16: e:\DJGPP\gnu\awk-1.3-3\
      09 Dec 1997 - 17:55:20: e:\DJGPP\share\man\man1\awk.1
<DIR> 01 Sep 2014 - 17:49:32: f:\MingW32\msys32\var\lib\pacman\local\awk-1.6-1\
```

C-source included in ./src. Makefiles for MinGW, Cygwin, Watcom and MSVC. Use at own
risk. Enjoy!

  Gisle Vanem <gvanem@yahoo.no>.

-------------------------------------------------------------------

### Changes:
```

  0.1:  Initial version.

  0.2:  Added file date/time stamp. Check for suffix or trailing
        wildcard in file-specification. If not found add a trailing "*".

  0.3: Handled the case where an env-var contains the current directory.
        E.g. when "PATH=./;c:\util", turn the "./" into CWD (using 'getcwd()')
        for 'stat("/.")' to work. Turn off command-line globbing in MinGW
        ('_CRT_glob = 0').

  0.4:  Rudimentary check for Python 'sys.path' searching.

  0.5:  Add a directory search spec mode ("--dir foo*.bar" searches for
        directories only). Better handling of file-specs with a sub-directory
        part. E.g. "envtool --python win32\Demos\Net*".

  0.6:  Add support for POSIX-style file matching (using fnmatch() from djgpp).
        E.g. a file-spec can contain things like "foo/[a-d]bar".
        Note: it doesn't handle ranges on directories. Only ranges on directories
              withing Pyhon EGG-files are handled.

        Improved Python 'sys.path' searching. Uses zipinfo.exe and looks inside
        Python EGG-files (zip files) for a match.

  0.7:  Improved Python 'sys.path' searching. Look inside Python EGGs and ZIP-files
        for a match (this uses the zipinfo external program).
        Add colour-output option '-C'.

  0.8:  New option '--pe' outputs version info from resource-section. E.g.:
          envtool --path --pe vcbuild.*
          Matches in %PATH:
               19 Mar 2010 - 15:02:22: g:\vc_2010\VC\vcpackages\vcbuild.dll
               ver 10.0.30319.1

  0.9:  Cosmetic changes in debug-output ('-d') and command-line parsing.

  0.92: Added option "--evry" to check matches in Everything's database.
        This option queries the database via IPC.
        Ref. http://www.voidtools.com/support/everything/

  0.93: The "--python" option now loads Python dynamically (pythonXX.dll)
        and calls 'PyRun_SimpleString()' to execute Python programs.

  0.94: Fixes for '--evry' (EverThing database) searches.
        Drop '-i' option.
        Add  '-r' option.

  0.95: Tweaks for better 'fnmatch()' matches.
        Improved '--evry' Regular Expression (--regex) searches.
        Build the MSVC-version using '-MT' (drop the dependency on MSVC1*.DLL)

  0.96: Better Python embedding; lookup python.exe in env-var %PYTHON, then on
        %PATH. Test for correct Python DLL. Report full name of python.exe in
        "envtool -V".

  0.97: Lots of improvements. Print more details on "envtool -VV" (compiler and
        linker flags).

  0.98: Added option "--man" to search for matches in all *subdir* of %MANPATH%.
        E.g. subdirs "man[1-9]" and "cat[1-9]".

        Added option "--cmake" to search for matches along the built-in Cmake
        module path and '%CMAKE_MODULE_PATH%'.

  0.99: Option "--pe" now calls WinTrust functions (in win_trust.c) to check if
        a PE-file is "Verified", "Not trusted" etc.

  1.0:  Added option "--pkg" to search in pkg-config's searcch-path specified by
        %PKG_CONFIG_PATH%.

```

PS. This file is written with the aid of [MarkdownPad](http://www.markdownpad.com/).
A real time-saver.
