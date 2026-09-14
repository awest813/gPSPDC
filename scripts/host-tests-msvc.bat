@echo off
rem Build and run the 13 host C test programs with MSVC, for machines without
rem gcc and make (tests\Makefile is the canonical runner). Set VCVARS to point
rem at a different vcvars64.bat. Binaries go to %TEMP%\gpspdc-host-tests.
rem Run save_io_test.py separately with Python and --cc cl in an MSVC environment.
setlocal
if "%VCVARS%"=="" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul
set "OUT=%TEMP%\gpspdc-host-tests"
if not exist "%OUT%" mkdir "%OUT%"
rem Tests read sources through ../ paths, so they run from tests\.
cd /d "%~dp0..\tests"
set FAIL=0
for %%T in (sh4_helpers_behavior_test phase1_helpers_test sh4_emit_encoding_test sh4_integration_contract_test phase2_ldm_stm_test phase3_memory_test phase4_cheats_test phase5_user_readiness_test phase6_release_verification_test dc_build_contract_test phase8_dreamcast_polish_test phase9_stability_test phase10_external_validation_test) do (
  cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Dpopen=_popen /Dpclose=_pclose /Ishims /I.. /Fe"%OUT%\%%T.exe" /Fo"%OUT%\%%T.obj" %%T.c >"%OUT%\%%T.build.log" 2>&1
  if errorlevel 1 (
    echo BUILD FAIL %%T - see %OUT%\%%T.build.log
    set FAIL=1
  ) else (
    "%OUT%\%%T.exe" >"%OUT%\%%T.run.log" 2>&1
    if errorlevel 1 (
      echo RUN FAIL %%T - see %OUT%\%%T.run.log
      set FAIL=1
    ) else (
      echo pass %%T
    )
  )
)
echo overall_fail=%FAIL%
exit /b %FAIL%
