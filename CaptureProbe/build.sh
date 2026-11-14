#!/bin/sh
# 构建 CaptureProbe（x86 + x64）。不进 sln，直接用 cl.exe —— 与
# DzjsTrainerInjector64/build.sh 同一套用法，沙箱里 MSBuild 被拦时也能跑。
#
# 产物：<repo>/Release/captureprobe-<arch>/CaptureProbe<arch>.exe
set -e
export MSYS2_ARG_CONV_EXCL='*'

ROOT="D:/原桌面/dzjstools/projects/jiyu"
SRC="$ROOT/CaptureProbe"
VC='D:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
KIT='D:\Windows Kits\10'
SDKVER='10.0.28000.0'

build_arch()
{
	ARCH="$1"
	case "$ARCH" in
		x86) HOSTDIR='Hostx64/x86'; LIBARCH='x86' ;;
		x64) HOSTDIR='Hostx64/x64'; LIBARCH='x64' ;;
		*) echo "unknown arch $ARCH"; exit 1 ;;
	esac

	CL="/d/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/$HOSTDIR/cl.exe"
	export INCLUDE="$VC\\include;$KIT\\Include\\$SDKVER\\ucrt;$KIT\\Include\\$SDKVER\\um;$KIT\\Include\\$SDKVER\\shared"
	export LIB="$VC\\lib\\$LIBARCH;$KIT\\Lib\\$SDKVER\\ucrt\\$LIBARCH;$KIT\\Lib\\$SDKVER\\um\\$LIBARCH"

	OUT="$ROOT/Release/captureprobe-$ARCH"
	mkdir -p "$OUT"

	echo "=== $ARCH ==="
	"$CL" -nologo -W3 -O2 -MT -EHsc -utf-8 \
		-DUNICODE -D_UNICODE -DWIN32 \
		-D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000000 \
		-DNOMINMAX -D_CRT_SECURE_NO_WARNINGS \
		-Fe:"$OUT/CaptureProbe-$ARCH.exe" -Fo:"$OUT/" \
		"$SRC/CaptureProbe.cpp" \
		"$SRC/ProbeMethods.cpp" \
		"$SRC/ProbeAnalyze.cpp" \
		"$SRC/VideoWall.cpp" \
		"$SRC/ImageIo.cpp" \
		-link -SUBSYSTEM:WINDOWS \
		user32.lib gdi32.lib shell32.lib ole32.lib windowscodecs.lib d3d11.lib dxgi.lib

	ls -l "$OUT/CaptureProbe-$ARCH.exe"
}

if [ $# -gt 0 ]; then
	build_arch "$1"
else
	build_arch x86
	build_arch x64
fi
