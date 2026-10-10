@echo off
rem Starts qbspserver on a map and leaves it waiting for receive.bat. Close it with Ctrl+C.
rem   server.bat                          -> testmap\e1m1.map
rem   server.bat path\to\mymap.map        -> another map
rem   set QBSPSERVER_PORT=26601           -> another port (set the same one for receive.bat)
rem   set QBSP_ARGS=-bsp2 -nopercent      -> qbsp options, used for every compile (default: -bsp2)
setlocal
set ROOT=%~dp0
set BIN=%ROOT%ericw-tools\build-windows\qbspserver\Release
set MAP=%~1
if "%MAP%"=="" set MAP=%ROOT%testmap\e1m1.map
if not defined QBSP_ARGS set QBSP_ARGS=-bsp2
set PORT_ARG=
if defined QBSPSERVER_PORT set PORT_ARG=-port %QBSPSERVER_PORT%

if not exist "%BIN%\qbspserver.exe" (
    echo [server] qbspserver.exe not found. Build it first: build.bat qbspserver
    exit /b 1
)

"%BIN%\qbspserver.exe" %PORT_ARG% %QBSP_ARGS% "%MAP%"
exit /b %ERRORLEVEL%
