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
:: Otherwise:
::   Since only Python2.7 is in AppVeyor's PATH by default, add this
::   Python 3.4 (x86) to the PATH.
::
::
if "%APPVEYOR_BUILD_FOLDER%" == "" set APPVEYOR_BUILD_FOLDER=%CD%

set PATH=%PATH%;c:\Python34

if %1. == build. goto build
if %1. == test.  goto test

echo Usage: %0 "build / test"
goto :EOF

:build
  cd %APPVEYOR_BUILD_FOLDER%\src
  msbuild -nologo -p:Configuration=Release -p:Platform="Win32" envtool.sln
  goto :EOF

::
:: Run some "envtool" tests.
::
:test
  set COLUMNS=120
  set APPDATA=%APPVEYOR_BUILD_FOLDER%

  call :create_auth_files

  cd %APPVEYOR_BUILD_FOLDER%\src

  echo Testing version output
  envtool -VVV

  echo Testing test output (show owner in test_PE_wintrust())
  envtool --test --owner

  echo Testing Python2 test output
  envtool --test --python=py2

  echo Testing Python3 test output
  envtool --test --python=py3

  echo Testing ETP-searches (should fail)
  envtool -d --test --evry:ftp.github.com:21

  echo Testing check output
  envtool --check

  :: echo Testing win_glob
  :: win_glob -fr "c:\Program Files (x86)\CMake"

  goto :EOF

::
:: Create a '.netrc' and '.authinfo' files for testing of 'src/auth.c' functions
::
:create_auth_files
  echo Creating '%APPDATA%/.netrc'.

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

  goto :EOF

