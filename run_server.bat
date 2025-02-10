@echo off
setlocal

:: Run the Python patch script
echo Running the server
build\bin\Release\MaterialXView.exe --refresh 100

echo 
endlocal
