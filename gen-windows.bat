@echo off
title Generate V3KN project files

REM Generate project files for your last Visual Studio version you have
call cmake -S . -B build
pause
