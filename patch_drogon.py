import os
import platform

def patch_cmake_file(cmake_file_path):
    try:
        with open(cmake_file_path, 'r', encoding='utf-8') as file:
            content = file.read()
    except Exception as e:
        print(f"Error reading {cmake_file_path}: {e}")
        return False

    original_content = content

    # Perform the replacements:
    content = content.replace("Jsoncpp_lib", "jsoncpp_lib")
    content = content.replace("find_package(Jsoncpp REQUIRED)", "find_package(jsoncpp REQUIRED)")
    content = content.replace("JSONCPP_INCLUDE_DIRS", "jsoncpp_INCLUDE_DIRS")

    # Write back only if there were changes
    if content != original_content:
        try:
            with open(cmake_file_path, 'w', encoding='utf-8') as file:
                file.write(content)
            print(f"Patched {cmake_file_path}")
        except Exception as e:
            print(f"Error writing {cmake_file_path}: {e}")
            return False
    else:
        print(f"No changes needed in {cmake_file_path}")
    return True

def main():
    # Only apply the patch on Windows.
    if platform.system() != "Windows":
        print("Not running on Windows. Skipping patch.")
        return

    # Define the path to the drogon folder relative to the script's directory.
    drogon_dir = os.path.join("source", "MaterialXView", "drogon")
    if not os.path.isdir(drogon_dir):
        print(f"Drogon directory not found: {drogon_dir}")
        return

    # Define the path to the CMakeLists.txt inside the drogon folder.
    cmake_file = os.path.join(drogon_dir, "CMakeLists.txt")
    if not os.path.isfile(cmake_file):
        print(f"CMakeLists.txt not found in {drogon_dir}")
        return

    # Patch the CMakeLists.txt file.
    if patch_cmake_file(cmake_file):
        print("Patch applied successfully.")
    else:
        print("Failed to apply patch.")

if __name__ == "__main__":
    main()
