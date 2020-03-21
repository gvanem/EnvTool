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
::
if "%APPVEYOR_BUILD_FOLDER%" == "" set APPVEYOR_BUILD_FOLDER=%~dp0
set PATH=%PATH%;c:\Python34
set PROMPT=$P$G

if %1. == build. goto build
if %1. == test.  goto test

echo Usage: %~dp0appveyor-script.bat "build / test"
exit /b 1

:build
  cd %APPVEYOR_BUILD_FOLDER%\src
  set WK_VER=8.1
  msbuild -nologo -p:Configuration=Release -p:Platform="Win32" envtool.sln
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

  cd %APPDATA%
  call :create_auth_files

  cd %APPDATA%\src

  echo on

  @echo Testing version output
  .\envtool -VVV

  @echo.
  @echo Testing test output (show owner in test_PE_wintrust())
  .\envtool --test --owner

  @echo.
  @echo Testing Python2 test output
  .\envtool --test --python=py2

  @echo.
  @echo Testing Python3 test output
  .\envtool --test --python=py3

  @echo.
  @echo Testing VCPKG output
  .\envtool --vcpkg azure-u*

  @echo.
  @echo Testing ETP-searches (should fail)
  .\envtool -d --test --evry:ftp.github.com:21

  @echo.
  @echo Testing verbose check output
  .\envtool --check -v

  :: echo Testing win_glob
  :: win_glob -fr "c:\Program Files (x86)\CMake"

  @echo off
  del /q %APPDATA%\.netrc %APPDATA%\.authinfo
  exit /b

::
:: Create a '<root>\.netrc' and '<root>\.authinfo' files for testing of 'src/auth.c' functions
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
  echo.
  goto :EOF

