@echo off
rem Compile benchmark (tools\bench.py): times recompiles through qbspserver after typical edits
rem (no change, 1 entity, 1 texture, 1 brush moved / added, a few of each, many changes), checks
rem the BSPs against a plain qbsp, and logs each run to compare versions.
rem   bench.bat "what changed"            -> maps small + e3m4, 3 repetitions (~15 min), logged
rem   bench.bat "what changed" quick      -> small only, 1 repetition (~1 min), not logged
rem   set BENCH_ARGS=--maps all --reps 5  -> any other tools\bench.py option (see bench.py --help)
rem Results: testmap\bench_history.txt (one line per run), bench_details.txt (phases, checks),
rem bench_history.jsonl (all numbers). Build first: build.bat qbspserver, qbspreceive, qbsp.
setlocal
set ROOT=%~dp0
set LABEL=%~1
if "%LABEL%"=="" set LABEL=-
set MODE=
if /i "%~2"=="quick" set MODE=--maps small --reps 1 --warmup 5 --no-log
python -u "%ROOT%tools\bench.py" --label "%LABEL%" %MODE% %BENCH_ARGS%
exit /b %ERRORLEVEL%
