@echo off
setlocal

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

:: Create the build directory if it doesn't exist
:: lets remove the build and create from scratch



if exist "build" (
    rmdir /s /q "build"
)

mkdir build


:: Change to the build directory
cd build

:: Detect the Conan profile (forcing detection)
conan profile detect --force
:: Install dependencies with Conan, forcing Release build for dependencies.
:: (Even though we generate a multi-config solution, we build dependencies in Release mode.)
conan install .. -s compiler="msvc" -s compiler.version=193 -s compiler.cppstd=17 -s build_type=Release --output-folder . --build=missing

echo Conan setup completed.

:: Configure the project with CMake
:: Do not specify -DCMAKE_BUILD_TYPE, so that a multi-config solution is generated.
cmake .. -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DMATERIALX_BUILD_VIEWER=ON -DCMAKE_POLICY_DEFAULT_CMP0091=NEW

:: Build only the Release configuration
cmake --build . --config Release --parallel

echo Build Complete
endlocal
