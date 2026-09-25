#!/bin/bash
cd /d/DevEcoStudioProjects/suplayer/tools/ffmpeg-build/openh264-2.5.0 || exit 1
echo "=== platform-gnu-chain.mk ==="
cat build/platform-gnu-chain.mk
echo "=== x86-common.mk ==="
cat build/x86-common.mk
