@echo off
::
:: AppVeyor .bat-file to run "build" or "test".
::
:: Do not confuse 'appveyor.yml' by the 'cd' below.
::
setlocal
set APPVEYOR_BUILD=0

::
:: One could also run this .BAT-file locally before testing with AppVeyor.
:: Then the env-var '%APPVEYOR_BUILD_FOLDER%' will not exist.
::
:: Otherwise when running on AppVeyor:
::   Since only Python2.7 is in AppVeyor's PATH by default, add this
::   Python 3.4 (x86) to the PATH.
::
if not "%APPVEYOR_BUILD_FOLDER%" == "" set APPVEYOR_BUILD=1
if "%APPVEYOR_BUILD_FOLDER%" == "" set APPVEYOR_BUILD_FOLDER=%~dp0
set PATH=%PATH%;c:\Python34;c:\msys64\MinGW64\bin
set PROMPT=$P$G

if %1. == build. goto build
if %1. == test.  goto test

echo Usage: %~dp0appveyor-script.bat "build / test"
exit /b 1

:build
  cd %APPVEYOR_BUILD_FOLDER%\src
  set WK_VER=8.1
  call nmake -nologo -f Makefile.VC all
  exit /b

::
:: Run some "envtool" tests.
::
:test
  ::
  :: For 'test_searchpath()' and 'NeedCurrentDirectoryForExePathA()'
  ::
  set NoDefaultCurrentDirectoryInExePath=1
  set APPDATA=%APPVEYOR_BUILD_FOLDER%
  set INCLUDE=%INCLUDE%;c:\Python34\include
  set VCPKG_ROOT=c:\Tools\vcpkg
  set COLUMNS=120

  cd %APPDATA%
  call :create_auth_files

  cd %APPDATA%\src

  ::
  :: Delete the quite limited Msys based Python; doesn't even have a 'pip' module
  ::
  del /q c:\msys64\MinGW64\bin\python.exe 2> NUL

  ::
  :: If running on Github Actions', delete this Python since it shadows for
  :: 'c:\ProgramData\Chocolatey\bin\python.exe'
  ::
  del /q c:\hostedtoolcache\windows\Python\3.7.9\x64\python.exe 2> NUL

  copy ..\CI\winsqlite3.dll > NUL

  echo on

  @call :green_msg Testing version output:
  .\envtool -VVV

  @call :green_msg Testing grep search and --inc mode:
  .\envtool --inc --grep PyOS_ pys*.h

  @call :green_msg Testing test output (show owner in test_PE_wintrust()):
  .\envtool --test --owner

  @call :green_msg Testing Python2 test output:
  .\envtool --test --python=py2 --verbose

  @call :green_msg Testing Python3 test output:
  .\envtool --test --python=py3 --verbose

  @call :green_msg Testing VCPKG output:
  .\envtool --vcpkg=all azure-u*

  @call :green_msg Testing ETP-searches (should fail):
  .\envtool -d --test --evry:ftp.github.com:21

  @call :green_msg Testing verbose check output:
  .\envtool --check --verbose

  :: @call :green_msg Testing win_glob:
  :: .\win_glob -fr "c:\Program Files (x86)\CMake"

  @call :green_msg Showing last 20 lines of cache-file:\n
  @"c:\Program Files\Git\usr\bin\tail" --lines=20 %TEMP%\envtool.cache

  @echo off
  del /q %APPDATA%\.netrc %APPDATA%\.authinfo winsqlite3.dll
  exit /b

::
:: Create a '<root>\.netrc' and '<root>\.authinfo' files for testing of 'src/auth.c' functions
::
:create_auth_files
  @call :green_msg Creating '%APPDATA%/.netrc'.
  echo #                                                                     > .netrc
  echo # This .netrc file was generated from "appveyor-script.bat".         >> .netrc
  echo #                                                                    >> .netrc
  echo machine host1  login user1     password password1                    >> .netrc
  echo machine host2  login user2     password password2                    >> .netrc
  echo default        login anonymous password your@email.address           >> .netrc

  @call :green_msg Creating '%APPDATA%/.authinfo'.
  echo #                                                                     > .authinfo
  echo # This .authinfo file was generated from "appveyor-script.bat".      >> .authinfo
  echo #                                                                    >> .authinfo
  echo machine host1  port 8080 login user1     password password1          >> .authinfo
  echo machine host2  port 7070 login user2     password password2          >> .authinfo
  echo default        port 21   login anonymous password your@email.address >> .authinfo
  goto :EOF

::
:: Use an 'echo.exe' with colour support in this sub-routine.
::
:green_msg
  @c:\msys64\usr\bin\echo.exe -e -n "\n\e[1;32m%*\e[0m"
  @goto :EOF
