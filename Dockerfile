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
    ccache \
    libgtk-3-dev \
  libunwind-dev \
    liblzma-dev \
    libglu1-mesa-dev \
    libxi6 libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
    gnupg \
  && rm -rf /var/lib/apt/lists/*

# Native deps for GStreamer H264 decoding
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
  && rm -rf /var/lib/apt/lists/*

# Some distributions do not ship a pkg-config file for libunwind even though
# the library is available. GStreamer's .pc file lists libunwind as a
# dependency, which causes pkg-config checks to fail. Provide small shim
# .pc files so CMake's pkg_check_modules can succeed.
RUN mkdir -p /usr/lib/aarch64-linux-gnu/pkgconfig /usr/lib/x86_64-linux-gnu/pkgconfig \
  && printf '%s\n' \
     'prefix=/usr' \
     'libdir=${prefix}/lib/aarch64-linux-gnu' \
     'Name: libunwind' \
     'Description: libunwind pkg-config shim' \
     'Version: 1' \
     'Libs: -lunwind' \
     'Cflags:' \
     > /usr/lib/aarch64-linux-gnu/pkgconfig/libunwind.pc \
  && printf '%s\n' \
     'prefix=/usr' \
     'libdir=${prefix}/lib/x86_64-linux-gnu' \
     'Name: libunwind' \
     'Description: libunwind pkg-config shim' \
     'Version: 1' \
     'Libs: -lunwind' \
     'Cflags:' \
     > /usr/lib/x86_64-linux-gnu/pkgconfig/libunwind.pc

    # Ensure pkg-config can find the shim .pc files for both architectures
    ENV PKG_CONFIG_PATH=/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/lib/pkgconfig

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

# Plain compiler vars for image-build steps (capnproto, etc.)
ENV CC=clang-${LLVM_VERSION}
ENV CXX=clang++-${LLVM_VERSION}

# Persist pub package cache across container runs.
ENV PUB_CACHE=/root/.pub-cache

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
  && git config --global --add safe.directory '*' \
  && flutter config --enable-linux-desktop \
  && flutter precache --linux \
  && flutter doctor -v

# Override CC/CXX with ccache wrappers for runtime builds.
# ccache dir is mounted as a named Docker volume at runtime.
ENV CCACHE_DIR=/root/.ccache
ENV CC="ccache clang-${LLVM_VERSION}"
ENV CXX="ccache clang++-${LLVM_VERSION}"

WORKDIR /workspace
CMD ["/bin/bash"]
