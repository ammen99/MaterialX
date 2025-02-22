@echo off
setlocal

:: Check for build configuration argument (Debug or Release)
if "%~1"=="" (
    echo No build configuration specified. Usage: vs_build.bat [Debug^|Release]
    exit /b 1
) else (
    set BUILD_TYPE=%~1
)

:: Validate build type
if /I not "%BUILD_TYPE%"=="Debug" if /I not "%BUILD_TYPE%"=="Release" (
    echo Invalid build configuration: %BUILD_TYPE%
    echo Valid configurations are: Debug or Release.
    exit /b 1
)

echo Build configuration: %BUILD_TYPE%

:: Run the Python patch script
echo Running patch_drogon.py...
python patch_drogon.py
if %errorlevel% neq 0 (
    echo Error applying patch.
    exit /b 1
)

:: Check if Conan is installed
where conan >nul 2>nul
if %errorlevel% neq 0 (
    echo Conan not found. Installing via pip...
    python -m pip install --upgrade pip
    python -m pip install conan
    if %errorlevel% neq 0 (
        echo Error: Failed to install Conan.
        exit /b 1
    )
)

:: Remove the build directory if it exists and create a new one
if exist "build" (
    rmdir /s /q "build"
)
mkdir build

:: Change to the build directory
cd build

:: Detect the Conan profile (forcing detection)
conan profile detect --force

:: Install dependencies with Conan using the specified build type
conan install .. -s compiler="msvc" -s compiler.version=193 -s compiler.cppstd=17 -s build_type=%BUILD_TYPE% --output-folder . --build=missing

echo Conan setup completed.

:: Configure the project with CMake (do not specify -DCMAKE_BUILD_TYPE for a multi-config solution)
cmake .. -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DMATERIALX_BUILD_VIEWER=ON -DCMAKE_POLICY_DEFAULT_CMP0091=NEW

:: Build using the specified configuration
cmake --build . --config %BUILD_TYPE% --parallel

echo Build Complete
endlocal
