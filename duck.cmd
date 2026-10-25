@echo off
python "%~dp0BuildTool\duck.py" %*
exit /b %errorlevel%
