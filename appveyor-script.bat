@echo off
::
:: AppVeyor .bat-file to run the "msbuild" program or the tests.
::
:: Do not confuse 'appveyor.yml' by the 'cd ' below.
::
setlocal

::
:: Since only AppVeyor's Python2.7 is in the PATH by default.
:: Add this Python 3.4 (x86).
::
set PATH=%PATH%;c:\Python34

if %1. == build. goto build
if %1. == test.  goto test

echo Usage: %0 "build / test"
exit /b 0

:build
  cd %APPVEYOR_BUILD_FOLDER%\src
  msbuild -nologo -p:Configuration=Release -p:Platform="Win32" envtool.sln
  exit /b 0

::
:: Run some "envtool" tests.
::
:test
  cd %APPVEYOR_BUILD_FOLDER%\src
  set COLUMNS=120
  echo Testing version output
  envtool -VVV

  echo Testing test output (show owner in test_PE_wintrust())
  envtool --test --owner

  echo Testing Python2 test output
  envtool --test --python=py2

  echo Testing Python3 test output
  envtool --test --python=py3

  echo Testing ETP-searches (should fail)
  envtool --test --evry:ftp.github.com

  echo Testing check output
  envtool --check

  :: echo Testing win_glob
  ::win_glob -fr "c:\Program Files (x86)\CMake"

