@echo off
chcp 65001 >nul
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
  echo 正在安装 Python 3.12 ...
  winget install -e --id Python.Python.3.12 --silent --accept-package-agreements --accept-source-agreements
  echo Python 装好后请关掉这个窗口，再双击一次"一键安装环境"。
  pause
  exit /b 0
)
set PYTHONIOENCODING=utf-8
python nx2h.py setup --fix
pause
