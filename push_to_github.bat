@echo off
set "PATH=E:\Git\cmd;%PATH%"
echo ============================================
echo  obs-ai-matte - push to GitHub
echo ============================================
echo.

REM --- OPTIONAL: paste a GitHub token between the quotes to skip login ---
set "TOKEN="

git remote remove origin 2>nul
if not "%TOKEN%"=="" (
  git remote add origin https://bukaisenqvq:%TOKEN%@github.com/bukaisenqvq/obs-ai-matte.git
) else (
  git remote add origin https://github.com/bukaisenqvq/obs-ai-matte.git
)
git branch -M main

echo.
echo Pushing... (if no token is set, a GitHub login window will appear)
echo.
git push -u origin main

if errorlevel 1 (
  echo.
  echo [!] Push failed.
  echo   - If asked for a password, GitHub no longer accepts your account
  echo     password. Use a Personal Access Token instead:
  echo       https://github.com/settings/tokens  (classic token, "repo" scope)
  echo     Either paste it as the password, or set TOKEN= above.
  echo   - Or sign in via the popup that appears.
)

echo.
echo Done. Press any key to close.
pause
