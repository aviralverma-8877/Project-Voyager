@echo off
rem Build voyager-link.exe (Windows, amd64, no console window) into dist\.
rem   build.bat            -> version from the VERSION file
rem   build.bat 1.0.0      -> version 1.0.0 (use the release tag)
setlocal
cd /d "%~dp0"
set VERSION=%1
if "%VERSION%"=="" set /p VERSION=<VERSION
set CGO_ENABLED=0
set GOOS=windows
set GOARCH=amd64
go vet ./... || exit /b 1
go test ./... || exit /b 1
go build -trimpath -ldflags "-s -w -H windowsgui -X main.version=%VERSION%" -o dist\voyager-link.exe . || exit /b 1
echo Built dist\voyager-link.exe (version %VERSION%)
