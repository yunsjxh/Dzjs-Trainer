#!/bin/sh
# 构建 64 位注入助手（不加入 sln，用 cl.exe 直接编成 x64 控制台程序）
# 产物: <repo>/x64/Release/DzjsTrainerInjector64.exe
set -e
# Git Bash 会把 /nologo 这类参数当路径转换，全部改用 - 前缀并禁用路径转换。
export MSYS2_ARG_CONV_EXCL='*'
ROOT="D:/原桌面/dzjstools/projects/jiyu"
CL="/d/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe"
MSVC='D:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
KIT='D:\Windows Kits\10'
export INCLUDE="$MSVC\\include;$KIT\\Include\\10.0.28000.0\\ucrt;$KIT\\Include\\10.0.28000.0\\um;$KIT\\Include\\10.0.28000.0\\shared"
export LIB="$MSVC\\lib\\x64;$KIT\\Lib\\10.0.28000.0\\ucrt\\x64;$KIT\\Lib\\10.0.28000.0\\um\\x64"
mkdir -p "$ROOT/x64/Release"
cd "$ROOT/DzjsTrainerInjector64"
"$CL" -nologo -W3 -O2 -MT -EHsc -utf-8 -DUNICODE -D_UNICODE -DWIN32 \
  -Fe:"$ROOT/x64/Release/DzjsTrainerInjector64.exe" \
  -Fo:"$ROOT/x64/Release/" \
  DzjsTrainerInjector64.cpp \
  -link -SUBSYSTEM:CONSOLE kernel32.lib shell32.lib
ls -l "$ROOT/x64/Release/DzjsTrainerInjector64.exe"
