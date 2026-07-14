#!/usr/bin/env python3
"""
Simple generator to populate .vscode JSON templates with platform-specific values
based on .vscode/platform.json. It replaces placeholders in the existing
.vscode JSON files and writes updated files back.

Placeholders supported:
- ${TOOLCHAIN_GCC}
- ${TOOLCHAIN_BIN_PATH}
- ${GDB_PATH}
- ${OPENOCD_PATH}
- ${BUILD_DIR}
- ${COMPILE_COMMANDS}
- ${STM32_DEVICE}
- ${STM32_TARGET}
- ${ELF_NAME}
- ${SVD_FILE}

Run from the workspace root: python3 .vscode/generate_vscode.py
"""
import json
import os
import platform
import re
import shutil
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '.'))
PLATFORM_FILE = os.path.join(ROOT, 'platform.json')
TEMPLATES = ['c_cpp_properties.json', 'launch.json', 'tasks.json', 'extensions.json']

# Embedded templates — the generator will write these files with platform values
EMBED_TEMPLATES = {
    'c_cpp_properties.json': '''{
    "configurations": [
        {
            "name": "Auto",
            "includePath": [
                "${workspaceFolder}/**"
            ],
            "defines": [
                "${STM32_DEVICE}",
                "USE_HAL_DRIVER"
            ],
            "compilerPath": "${TOOLCHAIN_GCC}",
            "cStandard": "c11",
            "cppStandard": "c++17",
            "intelliSenseMode": "gcc-arm",
            "compileCommands": "${COMPILE_COMMANDS}",
            "configurationProvider": "ms-vscode.cmake-tools"
        }
    ],
    "version": 4
}
''',

    'launch.json': '''{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Debug (OpenOCD)",
            "type": "cortex-debug",
            "request": "launch",
            "executable": "${workspaceFolder}/${BUILD_DIR}/Debug/${ELF_NAME}.elf",

            "servertype": "openocd",
            "gdbPath": "${GDB_PATH}",

            "configFiles": [
                "interface/stlink.cfg",
                "target/${STM32_TARGET}"
            ],

            "runToEntryPoint": "main",
            "svdFile": "${SVD_FILE}",
            "preLaunchTask": "CMake: Build (Debug)",
            "postDebugTask": "Kill OpenOCD"
        },
        {
            "name": "Attach (OpenOCD)",
            "type": "cortex-debug",
            "request": "attach",
            "executable": "${workspaceFolder}/${BUILD_DIR}/Debug/${ELF_NAME}.elf",

            "servertype": "openocd",
            "gdbPath": "${GDB_PATH}",

            "configFiles": [
                "interface/stlink.cfg",
                "target/${STM32_TARGET}"
            ],

            "svdFile": "${SVD_FILE}",
            "preLaunchTask": "CMake: Build (Debug)",
            "postDebugTask": "Kill OpenOCD"
        }
    ]
}
''',

    'tasks.json': '''{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "CMake: Configure (Debug)",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--preset",
                "Debug"
            ],
            "group": "build",
            "problemMatcher": [],
            "options": {
                "env": {
                    "PATH": "${TOOLCHAIN_BIN_PATH}:${env:PATH}"
                }
            }
        },
        {
            "label": "CMake: Build (Debug)",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--build",
                "${workspaceFolder}/${BUILD_DIR}/Debug",
                "--config",
                "Debug",
                "--target",
                "all",
                "-j",
                "10",
                "--verbose"
            ],
            "group": {
                "kind": "build",
                "isDefault": true
            },
            "problemMatcher": [
                "$gcc"
            ],
            "dependsOn": [
                "CMake: Configure (Debug)"
            ],
            "options": {
                "env": {
                    "PATH": "${TOOLCHAIN_BIN_PATH}:${env:PATH}"
                }
            }
        },
        {
            "label": "CMake: Clean",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--build",
                "${workspaceFolder}/${BUILD_DIR}/Debug",
                "--target",
                "clean"
            ],
            "group": "build",
            "problemMatcher": [],
            "options": {
                "env": {
                    "PATH": "${TOOLCHAIN_BIN_PATH}:${env:PATH}"
                }
            }
        },
        {
            "label": "CMake: Configure (Release)",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--preset",
                "Release"
            ],
            "group": "build",
            "problemMatcher": [],
            "options": {
                "env": {
                    "PATH": "${TOOLCHAIN_BIN_PATH}:${env:PATH}"
                }
            }
        },
        {
            "label": "CMake: Build (Release)",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--build",
                "${workspaceFolder}/${BUILD_DIR}/Release",
                "--config",
                "Release",
                "--target",
                "all",
                "-j",
                "10"
            ],
            "group": "build",
            "problemMatcher": [
                "$gcc"
            ],
            "dependsOn": [
                "CMake: Configure (Release)"
            ],
            "options": {
                "env": {
                    "PATH": "${TOOLCHAIN_BIN_PATH}:${env:PATH}"
                }
            }
        },
        {
            "label": "CMake: Configure (Debug-BareMetal)",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--preset",
                "Debug-BareMetal"
            ],
            "group": "build",
            "problemMatcher": [],
            "options": {
                "env": {
                    "PATH": "${TOOLCHAIN_BIN_PATH}:${env:PATH}"
                }
            }
        },
        {
            "label": "CMake: Build (Debug-BareMetal)",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--build",
                "${workspaceFolder}/${BUILD_DIR}/Debug-BareMetal",
                "--config",
                "Debug",
                "--target",
                "all",
                "-j",
                "10",
                "--verbose"
            ],
            "group": "build",
            "problemMatcher": [
                "$gcc"
            ],
            "dependsOn": [
                "CMake: Configure (Debug-BareMetal)"
            ],
            "options": {
                "env": {
                    "PATH": "${TOOLCHAIN_BIN_PATH}:${env:PATH}"
                }
            }
        },
        {
            "label": "CMake: Configure (Release-BareMetal)",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--preset",
                "Release-BareMetal"
            ],
            "group": "build",
            "problemMatcher": [],
            "options": {
                "env": {
                    "PATH": "${TOOLCHAIN_BIN_PATH}:${env:PATH}"
                }
            }
        },
        {
            "label": "CMake: Build (Release-BareMetal)",
            "type": "shell",
            "command": "cmake",
            "args": [
                "--build",
                "${workspaceFolder}/${BUILD_DIR}/Release-BareMetal",
                "--config",
                "Release",
                "--target",
                "all",
                "-j",
                "10"
            ],
            "group": "build",
            "problemMatcher": [
                "$gcc"
            ],
            "dependsOn": [
                "CMake: Configure (Release-BareMetal)"
            ],
            "options": {
                "env": {
                    "PATH": "${TOOLCHAIN_BIN_PATH}:${env:PATH}"
                }
            }
        },
        {
            "label": "Flash Firmware (Debug-BareMetal)",
            "type": "shell",
            "command": "${OPENOCD_PATH}",
            "args": [
                "-f",
                "interface/stlink.cfg",
                "-f",
                "target/${STM32_TARGET}",
                "-c",
                "program ${BUILD_DIR}/Debug-BareMetal/${ELF_NAME}.hex reset exit"
            ],
            "group": "build",
            "problemMatcher": [],
            "dependsOn": [
                "CMake: Build (Debug-BareMetal)"
            ],
            "options": {
                "cwd": "${workspaceFolder}"
            }
        },
        {
            "label": "Flash Firmware (Release-BareMetal)",
            "type": "shell",
            "command": "${OPENOCD_PATH}",
            "args": [
                "-f",
                "interface/stlink.cfg",
                "-f",
                "target/${STM32_TARGET}",
                "-c",
                "program ${BUILD_DIR}/Release-BareMetal/${ELF_NAME}.hex reset exit"
            ],
            "group": "build",
            "problemMatcher": [],
            "dependsOn": [
                "CMake: Build (Release-BareMetal)"
            ],
            "options": {
                "cwd": "${workspaceFolder}"
            }
        }
        ,
        {
            "label": "OpenOCD: Start GDB Server",
            "type": "shell",
            "command": "${OPENOCD_PATH}",
            "args": [
                "-f",
                "interface/stlink.cfg",
                "-f",
                "target/${STM32_TARGET}"
            ],
            "isBackground": true,
            "problemMatcher": [],
            "options": {
                "cwd": "${workspaceFolder}"
            }
        },
        {
            "label": "Kill OpenOCD",
            "type": "shell",
            "command": "${KILL_OPENOCD_CMD}",
            "args": [${KILL_OPENOCD_ARGS}],
            "problemMatcher": []
        }
    ]
}
''',

    'extensions.json': '''{
    "recommendations": [
        "ms-vscode.cpptools",
        "ms-vscode.cmake-tools",
        "marus25.cortex-debug",
        "ms-vscode.hexeditor"
    ],
    "unwantedRecommendations": []
}
'''
}

def load_platform_config():
    with open(PLATFORM_FILE, 'r', encoding='utf-8') as f:
        cfg = json.load(f)
    sysname = platform.system().lower()
    if 'linux' in sysname:
        key = 'linux'
    elif 'windows' in sysname:
        key = 'windows'
    else:
        key = 'linux'
    return cfg.get(key, cfg.get('linux'))

def detect_elf_name():
    """Auto-detect the ELF name from CMakeLists.txt by parsing CMAKE_PROJECT_NAME."""
    cmake_file = os.path.join(ROOT, 'CMakeLists.txt')
    if not os.path.exists(cmake_file):
        return None
    
    with open(cmake_file, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Look for set(CMAKE_PROJECT_NAME xxx)
    match = re.search(r'set\s*\(\s*CMAKE_PROJECT_NAME\s+([^\s\)]+)\s*\)', content)
    if match:
        return match.group(1)
    
    # Also try to find project(xxx)
    match = re.search(r'project\s*\(\s*([^\s\)]+)\s*\)', content)
    if match:
        project_name = match.group(1)
        # Check if it's a variable reference
        if project_name.startswith('${') and project_name.endswith('}'):
            return None  # Already handled by CMAKE_PROJECT_NAME
        return project_name
    
    return None

def replace_placeholders(text, mapping):
    for k, v in mapping.items():
        text = text.replace('${' + k + '}', v)
    return text

def main():
    cfg = load_platform_config()

    # Remove the Debug build directory if it exists
    build_dir_base = cfg.get('build_dir', 'build')
    debug_build_dir = os.path.join(ROOT, build_dir_base, 'Debug')
    if os.path.exists(debug_build_dir):
        print(f'Removing existing Debug build directory: {debug_build_dir}')
        shutil.rmtree(debug_build_dir)

    # Ensure .vscode directory exists
    vscode_dir = os.path.join(ROOT, '.vscode')
    if os.path.exists(vscode_dir):
        print(f'Removing existing files in .vscode directory: {vscode_dir}')
        for file_name in os.listdir(vscode_dir):
            file_path = os.path.join(vscode_dir, file_name)
            if os.path.isfile(file_path):
                os.remove(file_path)

    os.makedirs(vscode_dir, exist_ok=True)

    # Auto-detect ELF name from CMakeLists.txt if not specified
    elf_name = cfg.get('elf_name')
    if not elf_name:
        detected_elf = detect_elf_name()
        if detected_elf:
            elf_name = detected_elf
            print(f'Auto-detected ELF name from CMakeLists.txt: {elf_name}')
        else:
            elf_name = 'firmware'  # Fallback default
            print(f'Warning: Could not detect ELF name, using default: {elf_name}')
    
    # Extract toolchain bin path from toolchain_gcc if not explicitly set
    toolchain_bin_path = cfg.get('toolchain_bin_path')
    if not toolchain_bin_path:
        toolchain_gcc = cfg.get('toolchain_gcc', '')
        if toolchain_gcc:
            # Extract directory path (remove the gcc executable name)
            toolchain_bin_path = os.path.dirname(toolchain_gcc)
        else:
            toolchain_bin_path = ''
    
    # OS-aware kill command for OpenOCD (used by the Kill OpenOCD task)
    sysname = platform.system().lower()
    if 'windows' in sysname:
        kill_cmd = 'taskkill'
        kill_args = '"/IM","openocd.exe","/F"'
    else:
        kill_cmd = 'pkill'
        kill_args = '"-f","openocd"'

    mapping = {
        'TOOLCHAIN_GCC': cfg.get('toolchain_gcc', ''),
        'TOOLCHAIN_BIN_PATH': toolchain_bin_path,
        'GDB_PATH': cfg.get('gdb', ''),
        'OPENOCD_PATH': cfg.get('openocd', ''),
        'BUILD_DIR': cfg.get('build_dir', 'build/Debug'),
        'COMPILE_COMMANDS': cfg.get('compile_commands', '${workspaceFolder}/build/Debug/compile_commands.json'),
        'STM32_DEVICE': cfg.get('stm32_device', 'STM32L476xx'),
        'STM32_TARGET': cfg.get('stm32_target', 'stm32l4x.cfg'),
        'ELF_NAME': elf_name,
        'SVD_FILE': cfg.get('svd_file', '')
    }
    mapping['KILL_OPENOCD_CMD'] = kill_cmd
    mapping['KILL_OPENOCD_ARGS'] = kill_args

    for name in TEMPLATES:
        template = EMBED_TEMPLATES.get(name)
        if template is None:
            print('No embedded template for', name)
            continue
        new_text = replace_placeholders(template, mapping)
        path = os.path.join(vscode_dir, name)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(new_text)
        print('Wrote', path)

    print('Generation complete. Reload window in VS Code if necessary.')

if __name__ == '__main__':
    main()
