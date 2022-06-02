# EnvTool v1.4:

[![Build Status](https://ci.appveyor.com/api/projects/status/github/gvanem/envtool?branch=master&svg=true)](https://ci.appveyor.com/project/gvanem/envtool)
[![MinGW Build](https://github.com/gvanem/EnvTool/actions/workflows/gnu-make.yml/badge.svg)](https://github.com/gvanem/EnvTool/actions/workflows/gnu-make.yml)

A tool to search along various environment variables for files (or a wildcard). The following modes
handles these environment variables:
[![screenshot](envtool-help.png?raw=true)](envtool-help.png?raw=true)

<!--
| `--path` | `%PATH%` |
| :--- | :--- |
| `--inc` | `%INCLUDE%`, `%C_INCLUDE_PATH%` and `%CPLUS_INCLUDE_PATH%` |
| `--lib` | `%LIB%` and `%LIBRARY_PATH%` |
| `--cmake` | `%CMAKE_MODULE_PATH%` |
| `--man` | `%MANPATH%` |
| `--lua` | `%LUA_PATH%` and `%LUA_CPATH%` |
| `--pkg` | `%PKG_CONFIG_PATH%` |
| `--python` | `%PYTHONPATH%` and `sys.path[]` |
| `--evry` | [EveryThing](http://www.voidtools.com/support/everything) file database. |
| `--evry=host` | Remote search in a EveryThing database on `host`. |
| `--check` | check for missing directories in *all* supported environment variables and some Registry keys. |
-->

It also checks for missing directories along the above env-variables.

The `--path` option also checks these registry keys:
  `HKEY_CURRENT_USER\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths` and
  `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths`

and enumerates all keys for possible programs. E.g. if registry contains this:
  `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\makensis.exe` =
   `f:\MinGW32\bin\MingW-studio\makensis.exe`,

`envtool --path maken*` will include `f:\MinGW32\bin\MingW-studio\makensis.exe`
in the result.

Problem with old programs pestering your `PATH` and _Registry_ entries can be tricky
to diagnose. Here I had an problem with an old version of the _FoxitReader PDF reader_:
Checking with `envtool --path foxit*.exe`, resulted in:

```
  Matches in HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths:
   (2)  27 Nov 2014 - 10:24:04: f:\ProgramFiler\FoxitReader\FoxitReader.exe
  Matches in %PATH:
        21 Apr 2006 - 17:43:10: f:\util\FoxitReader.exe
   (2): found in "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths".
```

Hence if you write `FoxitReader` in the Window Run-box (_Winkey-R_), you'll get the
newer (27 Nov 2014) `FoxitReader` launched. But if you say `FoxitReader` in your shell
(cmd etc.), you'll get the old version (21 Apr 2006).

Other examples:

**E.g. 1**: `envtool --path notepad*.exe` first checks the `%PATH%` env-var
 for consistency (reports missing directories in `%PATH%`) and prints
 all the locations of `notepad*.exe`. On my box the result is:

```
Thu Jul 21 16:02:20 2011: f:\windows\system32\notepad-orig.exe
Mon Nov 18 19:26:40 2002: f:\windows\system32\notepad.exe
Thu Jul 21 16:13:11 2011: f:\windows\system32\notepad2.exe
Mon Nov 18 19:26:40 2002: f:\windows\notepad.exe
```

**E.g. 2**: `envtool --inc afxwin*` first checks the `%INCLUDE%` env-var
for consistency (reports missing directories in `%INCLUDE`) and prints
all the locations of `afxwin*`. On my box the result is:

```
Thu Apr 14 18:54:46 2005: g:\vc_2010\VC\AtlMfc\include\AFXWIN.H
Thu Apr 14 18:54:46 2005: g:\vc_2010\VC\AtlMfc\include\AFXWIN1.INL
Thu Apr 14 18:54:46 2005: g:\vc_2010\VC\AtlMfc\include\AFXWIN2.INL
Thu Apr 14 18:54:46 2005: g:\vc_2010\VC\AtlMfc\include\AFXWIN.H
Thu Apr 14 18:54:46 2005: g:\vc_2010\VC\AtlMfc\include\AFXWIN1.INL
Thu Apr 14 18:54:46 2005: g:\vc_2010\VC\AtlMfc\include\AFXWIN2.INL
```

**E.g. 3**: If an _App Paths_ registry key has an alias for a command, the target
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

**E.g. 4**: It's pretty amazing what the _FindFile()_ API in Windows can do. E.g.:
`envtool --path *-?++.exe`:

```
Tue Nov 19 12:01:38 2002: f:\Mingw32\bin\mingw32-c++.exe
Tue Nov 19 12:01:38 2002: f:\Mingw32\bin\mingw32-g++.exe
Wed Mar 09 14:39:05 2011: f:\CygWin\bin\i686-pc-cygwin-c++.exe
Wed Mar 09 14:39:05 2011: f:\CygWin\bin\i686-pc-cygwin-g++.exe
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

**E.g. 6**: The `--python` option will also look inside Python *EGGs* (plain ZIP-files) found
in `sys.path[]`. E.g.:
`envtool.exe --python socket.py`:

```
27 Mar 2013 - 16:41:58: stem\socket.py  (%PYTHONHOME\lib\site-packages\stem-1.0.1-py2.7.egg)
30 Apr 2014 - 09:54:04: f:\Programfiler\Python27\lib\socket.py
```

**E.g. 7**: If you have Lua installed, the `--lua` option will search in
`%LUA_PATH` and `%LUA_CPATH` for a match. E.g.:
`envtool.exe --lua [gs]*.dll`:

```
12 Aug 2021 - 13:11:04: f:\MingW32\src\inet\Lua\luasocket\install\gem.dll
12 Aug 2021 - 15:25:09: f:\MingW32\src\inet\Lua\LuaSec\src\ssl.dll
```

**E.g. 8**: The `--evry` option combined with the `--regex` (or `-r`) is quite powerful. To find
all directories with Unix man-pages, you can do this:
`envtool.exe --evry -r "man[1-9]$"`:

```
<DIR> 03 Jun 2014 - 17:56:08: f:\CygWin\usr\share\man\man1\
<DIR> 03 Jun 2014 - 17:56:08: f:\CygWin\usr\share\man\man3\
<DIR> 03 Jun 2014 - 17:56:08: f:\CygWin\usr\share\man\man5\
<DIR> 03 Jun 2014 - 17:56:08: f:\CygWin\usr\share\man\man7\
<DIR> 03 Jun 2014 - 17:56:08: f:\CygWin\usr\share\man\man8\
```
(assuming you have only 1 set of man-pages from a CygWin install)

Or to find only `foo*.bar` files under directory-branch(es) `misc`, you can do:
```
envtool.exe --evry -r "misc\\.*\\foo.*\.bar"
```

Or how much space is *wasted* in your Python cache-files. This would tell you quickly:
```
  envtool.exe --evry -sD __pycache__*
  ...
  592 matches found for "__pycache__*". Totalling 125 MB (131,391,488 bytes).
```

Or report all >= 500 MByte files on the `c:` partition:
```
  envtool --evry -s c:\* "size:>500MB"
  01 Feb 2022 - 07:10:28 -    3 GB: c:\hiberfil.sys
  27 Jan 2022 - 10:58:54 -    8 GB: c:\pagefile.sys
  ...
  6 matches found for "c:\* size:>500MB". Totalling 14 GB (14,585,290,752 bytes).
```

**E.g. 9**: More than one option-mode can combined. For example:
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

**E.g. 10**: All modes support showing the file-owner. Or only
specific owners.<br>
For example: `envtool.exe --path --owner=Admin* --owner=S-1-5* add*` could return:
```
Matches in %PATH:
      25 Jun 2015 - 17:46:39: Administratorer  f:\MingW32\TDM-gcc\bin\addr2line.exe
      01 Jun 2013 - 08:05:42: S-1-5-21...1001  f:\MingW32\bin\addr2line.exe
      17 Aug 2001 - 22:04:02: Administratorer  f:\ProgramFiler\Support-Tools\addiag.exe
      04 Nov 1999 - 09:45:44: Administratorer  f:\ProgramFiler\RKsupport\addiag.exe
      19 Jan 2018 - 00:12:23: Administratorer  f:\gv\dx-radio\UHD\host\bin\addr_test.pdb
```
The inverse `envtool.exe --path --owner=!Admin*` is also possible; showing files on the<br>
`%PATH` who's owner does *not* match `Admin*`.

**E.g. 11**: `envtool --check -v` does a shadow check for file-spec in these
  environment variables:<br>
   `PATH`, `LIB`, `LIBRARY_PATH`,`INCLUDE`, `C_INCLUDE_PATH`,
   `CPLUS_INCLUDE_PATH`, `MANPATH`,<br>
   `PKG_CONFIG_PATH`, `PYTHONPATH`,`CMAKE_MODULE_PATH`, `CLASSPATH` and `GOPATH`.

For all directories in an env-var, build lists of files matching a `file_spec` and do a shadow check of files
in all directories after each directory. This is to show possibly newer files (in "later" directories) that
should be used instead.

  E.g. with a `PATH=c:\ProgramFiler\Python27;c:\CygWin32\bin`
  and these files:
  ```
   c:\ProgramFiler\Python27\python.exe   24.06.2011  12:38   (oldest)
   c:\CygWin32\bin\python.exe            20.03.2019  18:32
  ```

  then the oldest `python.exe` shadows the newest `python.exe`.
  Situation such as thise can be tricky to diagnose, <br>
  but won't always hurt. If you get too many *shadow reports*, edit `%APPDATA%\envtool.cfg` like this:
```
[Shadow]
 ignore = f:\ProgramFiler\RKsupport\*.exe # ignore all .EXEs here
 ignore = unins00*.exe  # Ignore uninstall programs everywhere.
```

**E.g. 12**: `envtool --evry --signed chrome.dll` does a signature check of all
  `chrome.dll` files:

```
        18 May 2022 - 06:45:16: c:\Users\Gisle\AppData\Local\Google\Chrome\Application\102.0.5005.63\chrome.dll
        ver 102.0.5005.63, 64-bit, Chksum OK      (Verified, Google LLC).
        09 Jul 2004 - 12:16:08: f:\gv\dx-radio\WiNRADiO\DRMplayer\MMPlayerSR\components\chrome.dll
        ver 0.0.0.0, 32-bit, Chksum OK            (Not signed).
```

  Here the `--pe` option is implicitly turned on.

---

C-source included in `./src`. Makefiles for MinGW, Cygwin, clang-cl and MSVC.
Enjoy!

`#include <std_disclaimer.h>`:<br>
   *"I do not accept responsibility for any effects, adverse or otherwise,
    that this code may have on you, your computer, your sanity, your dog,
    and anything else that you can think of. Use it at your own risk."*


Gisle Vanem [gvanem@yahoo.no](mailto:gvanem@yahoo.no).


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

  0.94: Fixes for '--evry' (EveryThing database) searches.
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

  1.1:  Added option "--check" to check for missing directories in all supported
        environment variables.
        Added reading of an config-file; "%APPDATA%\envtool.cfg" is read at startup
        to support files to ignore. Copy the included "envtool.cfg" to your "%APPDATA"
        folder if needed.

  1.2:  Modifiers for "--evry" option is now possible. E.g.
          envtool --evry *.exe rc:today                     - find today's changes of all *.exe files.
          envtool --evry Makefile.am content:pod2man        - find Makefile.am with pod2man commands.
          envtool --evry M*.mp3 artist:Madonna "year:<2002" - find all Madonna M*.mp3 titles issued prior to 2002.

   1.3: Enhanced "envtool --check -v" to look for shadowed files in important environment variables
        like PATH, INCLUDE and LIB. See E.g. 10. above.

   1.4: Added a "--lua" option.

```

PS. This file is written with the aid of the **[Atom](https://atom.io/)**
editor and it's **[Markdown-Preview](https://atom.io/packages/markdown-preview)**.
A real time-saver.
