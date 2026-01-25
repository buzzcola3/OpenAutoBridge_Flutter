# Minimal reproducible builder for OpenAutoFlutter on Linux
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG LLVM_VERSION=18
ARG FLUTTER_VERSION=3.38.6
ARG FLUTTER_CHANNEL=stable
ARG FLUTTER_ARM64_SDK_URL=
ARG TARGETARCH
ENV FLUTTER_HOME=/opt/flutter
ENV PATH="${FLUTTER_HOME}/bin:${PATH}"

# Base tools and Flutter Linux desktop deps
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    git \
    xz-utils \
    unzip \
    zip \
    build-essential \
    pkg-config \
    ninja-build \
    cmake \
    libgtk-3-dev \
    liblzma-dev \
    libglu1-mesa-dev \
    libxi6 libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
    gnupg \
  && rm -rf /var/lib/apt/lists/*

# Native deps for FFmpeg codecs
RUN apt-get update && apt-get install -y --no-install-recommends \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    libswresample-dev \
  && rm -rf /var/lib/apt/lists/*

# LLVM/Clang + libc++
RUN echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" > /etc/apt/sources.list.d/llvm.list \
  && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | tee /etc/apt/trusted.gpg.d/llvm.asc >/dev/null \
  && apt-get update \
  && apt-get install -y --no-install-recommends \
       clang-${LLVM_VERSION} \
       lld-${LLVM_VERSION} \
       libc++-${LLVM_VERSION}-dev \
       libc++abi-${LLVM_VERSION}-dev \
  && ln -sf /usr/bin/clang-${LLVM_VERSION} /usr/bin/clang \
  && ln -sf /usr/bin/clang++-${LLVM_VERSION} /usr/bin/clang++ \
  && rm -rf /var/lib/apt/lists/*

ENV CC=clang-${LLVM_VERSION}
ENV CXX=clang++-${LLVM_VERSION}

# Cap'n Proto 1.1.0 (matches OpenAutoTransport prebuilt)
RUN curl -fsSL https://capnproto.org/capnproto-c++-1.1.0.tar.gz -o /tmp/capnp.tar.gz \
  && tar -xzf /tmp/capnp.tar.gz -C /tmp \
  && cd /tmp/capnproto-c++-1.1.0 \
  && ./configure --disable-shared CC=${CC} CXX=${CXX} \
  && make -j"$(nproc)" \
  && make install \
  && ldconfig \
  && rm -rf /tmp/capnp.tar.gz /tmp/capnproto-c++-1.1.0

# Flutter SDK
RUN mkdir -p /opt \
  && if [ "${TARGETARCH}" = "arm64" ]; then \
       if [ -n "${FLUTTER_ARM64_SDK_URL}" ]; then \
         curl -fsSL "${FLUTTER_ARM64_SDK_URL}" -o /tmp/flutter-arm64.zip \
         && unzip -q /tmp/flutter-arm64.zip -d /opt \
         && rm /tmp/flutter-arm64.zip; \
       else \
         git clone --depth 1 --branch "${FLUTTER_VERSION}" https://github.com/flutter/flutter.git /opt/flutter; \
       fi; \
     else \
       curl -fsSL https://storage.googleapis.com/flutter_infra_release/releases/${FLUTTER_CHANNEL}/linux/flutter_linux_${FLUTTER_VERSION}-${FLUTTER_CHANNEL}.tar.xz \
         | tar -xJ -C /opt; \
     fi \
  && if [ ! -d /opt/flutter ] && [ -d /opt/Flutter-SDK-ARM64 ]; then \
       mv /opt/Flutter-SDK-ARM64 /opt/flutter; \
     fi

# Precache Linux artifacts and validate
RUN git config --global --add safe.directory /opt/flutter \
  && flutter config --enable-linux-desktop \
  && flutter precache --linux \
  && flutter doctor -v

WORKDIR /workspace
CMD ["/bin/bash"]
