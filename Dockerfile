# ============================================================================
# Stage 1: Build C++ server
# ============================================================================
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq \
    build-essential git curl zip unzip tar pkg-config \
    libssl-dev python3 python3-pip python3-venv \
    && rm -rf /var/lib/apt/lists/*

# Install cmake >= 4.3.3 (vcpkg requirement)
RUN pip3 install cmake --break-system-packages

ARG VCPKG_CACHE_BUST=1
# Install vcpkg
RUN git clone --depth 1 https://github.com/microsoft/vcpkg.git /vcpkg \
    && /vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/vcpkg
ENV CMAKE_TOOLCHAIN_FILE=/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build Codis
WORKDIR /build
COPY CMakeLists.txt vcpkg.json ./
COPY packages/ packages/
RUN cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE} \
    && cmake --build build -- -j$(nproc)

# ============================================================================
# Stage 2: Runtime
# ============================================================================
FROM ubuntu:24.04

RUN apt-get update -qq && apt-get install -y -qq \
    libssl3 libsqlite3-0 ca-certificates python3 python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Python dependencies (Feishu SDK)
RUN pip3 install --break-system-packages lark-oapi httpx

# Copy C++ binaries
COPY --from=builder /build/build/packages/server/opencode-server /usr/local/bin/
COPY --from=builder /build/build/packages/cli/opencode /usr/local/bin/

# Copy config + bot
WORKDIR /app
COPY config/config.toml config/
COPY bot/feishu_bot.py bot/

# Entrypoint
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENV CODIS_SERVER=http://127.0.0.1:8711
ENV SERVER_PORT=8711

EXPOSE 8711

ENTRYPOINT ["/entrypoint.sh"]
