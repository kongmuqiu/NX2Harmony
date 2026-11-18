@echo off
chcp 65001 >nul
cd /d "%~dp0"
python -c "import PySide6, cryptography" 2>nul || (
  echo 第一次打开：正在安装界面库 PySide6 和 cryptography（约 80 MB）...
  python -m pip install --user -r "%~dp0requirements.txt"
)
start "" pythonw "%~dp0NX2Harmony.pyw"
