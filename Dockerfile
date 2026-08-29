# syntax=docker/dockerfile:1
#
# Toolchain image for building the PiSTorm ATARI JIT emulator.
#
# The Amiberry JIT backend is AArch64-only, so this build MUST run on
# linux/arm64. Docker Desktop on Apple Silicon does this natively; anywhere
# else you need an arm64 toolchain or QEMU emulation.
#
# This image only provides the build tools - it contains no source. `make
# docker` runs it with the working tree bind-mounted and compiles in-tree, so
# only changed translation units recompile. Because the Makefile builds
# in-tree, the .o/.d files and the emulator/ataritest binaries appear in YOUR
# working tree - all gitignored, and `make clean` (works on the host too)
# removes them.
#
# The result is a Raspberry Pi binary - copy it to the Pi, it cannot run on
# the build machine. No hardware-specific or V4L2-FFmpeg bits are built here:
# `make ffmpeg` stays a Pi-side step (VIDEO.md).

FROM debian:trixie

ARG DEBIAN_FRONTEND=noninteractive

# Build dependency list mirrors README.md "Preparation". cmake/git/ffmpeg and
# the runtime tools are not needed to compile, so they are left out.
# Nothing here depends on the source tree, so this image is cached and only
# rebuilt when the Dockerfile or this list changes.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++ make pkg-config \
        libsdl3-dev libmpg123-dev libjpeg-dev libdrm-dev libslirp-dev \
        zlib1g-dev \
        libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
        libswresample-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
