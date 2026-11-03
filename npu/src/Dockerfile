# Toolchain that built the shipped fsr4npud and fsr4npu-check: Ubuntu 24.04 x86-64 with the aarch64 GNU cross compiler
# (g++ 13.3.0, glibc 2.39 cross headers, binutils 2.42) and the Vulkan headers 1.3.275 (libvulkan-dev, arm64).
# fsr4npud.cpp includes <vulkan/vulkan.h> but loads libvulkan.so.1 at run time (dlopen), so only the headers are needed.
#   docker build -t vela-npu-build npu/src
#   docker run --rm -v <repo>/npu/src:/src -v <sdk>:/sdk -e QNN_SDK_ROOT=/sdk -e OUT=/out -v <out>:/out vela-npu-build sh /src/build.sh
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture arm64 \
 && sed -i 's|^URIs: http://archive.ubuntu.com/ubuntu/|URIs: http://archive.ubuntu.com/ubuntu/\nArchitectures: amd64|; s|^URIs: http://security.ubuntu.com/ubuntu/|URIs: http://security.ubuntu.com/ubuntu/\nArchitectures: amd64|' /etc/apt/sources.list.d/ubuntu.sources \
 && printf '\nTypes: deb\nURIs: http://ports.ubuntu.com/ubuntu-ports/\nSuites: noble noble-updates\nComponents: main universe\nArchitectures: arm64\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n' >> /etc/apt/sources.list.d/ubuntu.sources \
 && apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends g++-aarch64-linux-gnu libvulkan-dev:arm64 \
 && rm -rf /var/lib/apt/lists/*
