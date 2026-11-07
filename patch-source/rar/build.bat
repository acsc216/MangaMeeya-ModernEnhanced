@echo off
rem Build rar.spi (32-bit). Run from an x86 Visual Studio command prompt,
rem or use this script from a normal prompt (it calls vcvars32.bat).
rem
rem   build.bat            build into build\ and produce rar.spi
rem
setlocal
if not defined VCINSTALLDIR (
  call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
)
if not exist build mkdir build

rem The official UnRARDll project compiles these top-level files.  Files such
rem as unpack50.cpp, win32stm.cpp, recvol3.cpp and uicommon.cpp are #included
rem by others and must not be compiled separately.
cl /nologo /c /MT /O2 /EHsc /W0 /DRARDLL /DUNRAR /DSILENT /Iunrar /FIrar.hpp /Fobuild\ ^
  unrar\archive.cpp unrar\arcread.cpp unrar\blake2s.cpp unrar\cmddata.cpp ^
  unrar\consio.cpp unrar\crc.cpp unrar\crypt.cpp unrar\dll.cpp ^
  unrar\encname.cpp unrar\errhnd.cpp unrar\extinfo.cpp unrar\extract.cpp ^
  unrar\filcreat.cpp unrar\file.cpp unrar\filefn.cpp unrar\filestr.cpp ^
  unrar\find.cpp unrar\getbits.cpp unrar\hash.cpp unrar\headers.cpp ^
  unrar\isnt.cpp unrar\largepage.cpp unrar\match.cpp unrar\motw.cpp ^
  unrar\options.cpp unrar\pathfn.cpp unrar\qopen.cpp unrar\rarvm.cpp ^
  unrar\rawread.cpp unrar\rdwrfn.cpp unrar\rijndael.cpp unrar\rs.cpp ^
  unrar\rs16.cpp unrar\scantree.cpp unrar\secpassword.cpp unrar\sha1.cpp ^
  unrar\sha256.cpp unrar\smallfn.cpp unrar\strfn.cpp unrar\strlist.cpp ^
  unrar\system.cpp unrar\threadpool.cpp unrar\timefn.cpp unrar\ui.cpp ^
  unrar\unicode.cpp unrar\unpack.cpp unrar\volume.cpp || exit /b 1

rem global.cpp defines the EXTVAR symbols (INCLUDEGLOBAL) and must NOT get the
rem forced precompiled header, otherwise ErrHandler stays extern.
cl /nologo /c /MT /O2 /EHsc /W0 /DRARDLL /DUNRAR /DSILENT /Iunrar ^
  /Fobuild\global.obj unrar\global.cpp || exit /b 1

cl /nologo /c /MT /O2 /EHsc /W0 /Iunrar /Fobuild\plugin_rar.obj rar.cpp || exit /b 1
link /nologo /DLL /OUT:rar.spi /DEF:rar.def build\*.obj ^
  user32.lib crypt32.lib advapi32.lib shell32.lib ole32.lib || exit /b 1

echo.
echo Built rar.spi
