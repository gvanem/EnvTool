EnvTool v0.94:
--------------

  A simple tool that I initially put together in day and a half (but the
  features grew). It's purpose is simply to search and check various environment
  variables for missing directories. And check where a specific file (or wildcard)
  is in a corresponding environment variable. The `%PATH%`, `%INCLUDE%`,
  `%C_INCLUDE_PATH%`, `%CPLUS_INCLUDE_PATH%`, `%LIBRARY_PATH%`, `%LIB%` and/or
  `%PYTHONPATH%` variables are checked.

  The option **--path** also checks these registry keys:
    `HKEY_CURRENT_USER\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths` and
    `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths`

  and enumerates all keys for possible programs. E.g. if registry contains this:
    `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\makensis.exe` = 
     `g:\MinGW32\bin\MingW-studio\makensis.exe`,

  **envtool --path maken*** will include `g:\MinGW32\bin\MingW-studio\makensis.exe`
  in the result.

  Problem with old programs pestering your `PATH` and *Registry* entries can be tricky
  to diagnose. Here I had an problem with an old version of the "FoxitReader PDF reader":
  Checking with **envtool --path foxit*.exe**, resulted in:

    Matches in HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths:
     (2)  27 Nov 2014 - 10:24:04: g:\ProgramFiler\FoxitReader\FoxitReader.exe
    Matches in %PATH:
          21 Apr 2006 - 17:43:10: f:\util\FoxitReader.exe
     (2): found in "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths".

  Hence if you write `FoxitReader` in the Window Run-box (*Winkey-R*), you'll get the
  newer (27 Nov 2014) `FoxitReader` launched. But if you say `FoxitReader` in your shell
  (cmd etc.), you'll get the old version (21 Apr 2006).

  Other examples:

  E.g. 1: *envtool --path notepad*.exe** first checks the `%PATH%` env-var
    for consistency (reports missing directories in `%PATH%`) and prints
    all the locations of **notepad*.exe**. On my box the result is:

      Thu Jul 21 16:02:20 2011 : f:\windows\system32\notepad-orig.exe
      Mon Nov 18 19:26:40 2002 : f:\windows\system32\notepad.exe
      Thu Jul 21 16:13:11 2011 : f:\windows\system32\notepad2.exe
      Mon Nov 18 19:26:40 2002 : f:\windows\notepad.exe

  E.g. 2: **envtool --inc afxwin*** first checks the `%INCLUDE%` env-var
    for consistency (reports missing directories in `%INCLUDE`) and prints
    all the locations of **afxwin***. On my box the result is:

      Thu Apr 14 18:54:46 2005 : g:\vc_2010\VC\AtlMfc\include\AFXWIN.H
      Thu Apr 14 18:54:46 2005 : g:\vc_2010\VC\AtlMfc\include\AFXWIN1.INL
      Thu Apr 14 18:54:46 2005 : g:\vc_2010\VC\AtlMfc\include\AFXWIN2.INL
      Thu Apr 14 18:54:46 2005 : g:\vc_2010\VC\AtlMfc\include\AFXWIN.H
      Thu Apr 14 18:54:46 2005 : g:\vc_2010\VC\AtlMfc\include\AFXWIN1.INL
      Thu Apr 14 18:54:46 2005 : g:\vc_2010\VC\AtlMfc\include\AFXWIN2.INL

  E.g. 3: If an *App Paths* registry key has an alias for a command, the target
    program is printed. E.g. if:
```
      HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\winzip.exe
```
    points to
```    
      c:\PROGRA~1\WINZIP\winzip32.exe
```
    (here `winzip.exe` is an alias for the real program `winzip32.exe`). Hence
    **envtool --path winzip*** reports:
```
  Fri Oct 11 09:10:00 2002: G:\PROGRA~1\WINZIP\winzip.exe !
  Fri Oct 11 09:10:00 2002: G:\PROGRA~1\WINZIP\winzip32.exe !
  (!) - found in registry.
```

  E.g. 4: It's pretty amazing what the *FindFile()* API in Windows can do. E.g.:
    **envtool --path *-?++.exe**

      Tue Nov 19 12:01:38 2002 : g:\Mingw32\bin\mingw32-c++.exe
      Tue Nov 19 12:01:38 2002 : g:\Mingw32\bin\mingw32-g++.exe
      Wed Mar 09 14:39:05 2011 : g:\CygWin\bin\i686-pc-cygwin-c++.exe
      Wed Mar 09 14:39:05 2011 : g:\CygWin\bin\i686-pc-cygwin-g++.exe

  E.g. 5: If you have Python installed, the **--python** option will search in
    `%PYTHONPATH` and `sys.path[]` for a match. E.g.: 
    **envtool.exe --python ss*.py**

      24 Jun 2011 - 11:38:10: g:\ProgramFiler\Python27\lib\ssl.py
      16 Feb 2011 - 12:14:28: g:\ProgramFiler\Python27\lib\site-packages\win32\lib\sspi.py
      16 Feb 2011 - 12:14:28: g:\ProgramFiler\Python27\lib\site-packages\win32\lib\sspicon.py


C-source included in ./src. Makefiles for MingW, Watcom and MSVC. Use at own
risk. Enjoy!

  Gisle Vanem <gvanem@yahoo.no>.
