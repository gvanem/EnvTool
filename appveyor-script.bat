@echo off
::
:: AppVeyor .bat-file to run "build" or "test".
::
:: Do not confuse 'appveyor.yml' by the 'cd' below.
::
setlocal

::
:: One could also run this .BAT-file locally before testing with AppVeyor.
:: Then the env-var '%APPVEYOR_BUILD_FOLDER%' will not exist.
::
:: Otherwise when running on AppVeyor:
::   Since only Python2.7 is in AppVeyor's PATH by default, add this
::   Python 3.4 (x86) to the PATH.
::   And add the path to AppVeyor's dual-mode MinGW.
::
if "%APPVEYOR_BUILD_FOLDER%" == "" set APPVEYOR_BUILD_FOLDER=%~dp0
set PATH=%PATH%;c:\Python34;c:\msys64\MinGW64\bin
set PROMPT=$P$G

::
:: Use an 'echo.exe' with colour support.
::
set _ECHO=@c:\msys64\usr\bin\echo.exe -e

if %1. == build. goto build
if %1. == test.  goto test

echo Usage: %~dp0appveyor-script.bat "build / test"
exit /b 1

:build
  cd %APPVEYOR_BUILD_FOLDER%\src
  set WK_VER=8.1
  msbuild -nologo -p:Configuration=%REL_DBG% -p:Platform="Win32" envtool.sln
  exit /b

::
:: Run some "envtool" tests.
::
:test
  ::
  :: For 'test_searchpath()' and 'NeedCurrentDirectoryForExePathA()'
  ::
  set NoDefaultCurrentDirectoryInExePath=1
  set COLUMNS=120
  set APPDATA=%APPVEYOR_BUILD_FOLDER%
  set INCLUDE=%INCLUDE%;c:\Python34\include
  set VCPKG_ROOT=c:\Tools\vcpkg

  cd %APPDATA%
  call :create_auth_files

  cd %APPDATA%\src
  ::
  :: Delete the quite limited Msys based Python; doesn't even have a 'pip' module
  ::
  del /q c:\msys64\MinGW64\bin\python.exe

  echo on

  %_ECHO% "\e[1;32mTesting version output:\e[0m"
  .\envtool -VVV

  @echo.
  %_ECHO% "\e[1;32mTesting grep search and --inc mode:\e[0m"
  .\envtool --inc --no-gcc --no-g++ --no-clang --grep PyOS_ pys*.h

  @echo.
  %_ECHO% "\e[1;32mTesting test output (show owner in test_PE_wintrust()):\e[0m"
  .\envtool --test --owner

  @echo.
  %_ECHO% "\e[1;32mTesting Python2 test output:\e[0m"
  .\envtool --test --python=py2

  @echo.
  %_ECHO% "\e[1;32mTesting Python3 test output:\e[0m"
  .\envtool --test --python=py3

  @echo.
  %_ECHO% "\e[1;32mTesting VCPKG output:\e[0m"
  .\envtool --vcpkg=all azure-u*

  @echo.
  %_ECHO% "\e[1;32mTesting ETP-searches (should fail):\e[0m"
  .\envtool -d --test --evry:ftp.github.com:21

  @echo.
  %_ECHO% "\e[1;32mTesting verbose check output:\e[0m"
  .\envtool --check -v

  :: %_ECHO% "\e[1;32mTesting win_glob:\e[0m"
  :: win_glob -fr "c:\Program Files (x86)\CMake"

  @echo.
  %_ECHO% "\e[1;32mShowing last 20 lines of cache-file:\e[0m"
  @"c:\Program Files\Git\usr\bin\tail" --lines=20 %TEMP%\envtool.cache

  @echo off
  del /q %APPDATA%\.netrc %APPDATA%\.authinfo
  exit /b

::
:: Create a '<root>\.netrc' and '<root>\.authinfo' files for testing of 'src/auth.c' functions
::
:create_auth_files
  %_ECHO% "\e[1;32mCreating '%APPDATA%/.netrc'.\e[0m"

  echo #                                                                     > .netrc
  echo # This .netrc file was generated from "appveyor-script.bat".         >> .netrc
  echo #                                                                    >> .netrc
  echo machine host1  login user1     password password1                    >> .netrc
  echo machine host2  login user2     password password2                    >> .netrc
  echo default        login anonymous password your@email.address           >> .netrc

  echo Creating '%APPDATA%/.authinfo'.
  echo #                                                                     > .authinfo
  echo # This .authinfo file was generated from "appveyor-script.bat".      >> .authinfo
  echo #                                                                    >> .authinfo
  echo machine host1  port 8080 login user1     password password1          >> .authinfo
  echo machine host2  port 7070 login user2     password password2          >> .authinfo
  echo default        port 21   login anonymous password your@email.address >> .authinfo
  echo.
  goto :EOF

