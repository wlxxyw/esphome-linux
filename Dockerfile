# Dockerfile for building esphome-linux with nimble and bluez dependencies
FROM ubuntu:22.04 AS builder

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    python3 \
    python3-pip \
    ninja-build \
    pkg-config \
    gcc \
    g++ \
    make \
    cmake \
    git \
    wget \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Install latest Meson via pip (apt version is 0.61.2, we need >= 0.62.0)
RUN pip3 install --no-cache-dir meson

# Set working directory
WORKDIR /workspace

# Optional: set to "1" to disable Bluetooth proxy plugin
ARG SENSOR_ONLY=0

# Copy dependency build scripts and sources
COPY nimble/ nimble/
COPY bluez/ bluez/
COPY libble/ libble/

# Build nimble dependency (must be built first, as libble depends on it)
RUN cd nimble && \
    ./build.sh && \
    cd ..

# Build bluez dependency
RUN cd bluez && \
    ./build.sh && \
    cd ..

# Build libble dependency (depends on nimble)
RUN cd libble && \
    ./build.sh && \
    cd ..

# Copy project files
COPY . .

# Disable Bluetooth proxy plugin if SENSOR_ONLY is set
RUN if [ "$SENSOR_ONLY" = "1" ]; then \
      sed -i '/^option.*enable_bluetooth_proxy/,/^)/ s/value: true/value: false/' meson_options.txt && \
      echo "Bluetooth proxy DISABLED (sensor-only build)"; \
    fi

# Set TMPDIR to avoid macOS volume mount issues
ENV TMPDIR=/tmp

# Set PKG_CONFIG_PATH to find nimble, bluez, and libble libraries
ENV PKG_CONFIG_PATH=/workspace/nimble/out/lib/pkgconfig:/workspace/bluez/out/lib/pkgconfig:/workspace/libble/out/usr/lib/pkgconfig \
    LD_LIBRARY_PATH=/workspace/nimble/out/lib:/workspace/bluez/out/lib:/workspace/libble/out/usr/lib \
    CPATH=/workspace/nimble/out/include:/workspace/bluez/out/include:/workspace/libble/out/usr/include \
    LIBRARY_PATH=/workspace/nimble/out/lib:/workspace/bluez/out/lib:/workspace/libble/out/usr/lib

# Build the project
RUN meson setup build && \
    meson compile -C build

# Create dependencies archive with flat lib/include structure and license files
RUN mkdir -p /deps-archive/lib /deps-archive/include /deps-archive/licenses && \
    cp -r /workspace/nimble/out/lib/* /deps-archive/lib/ && \
    cp -r /workspace/nimble/out/include/* /deps-archive/include/ && \
    cp -r /workspace/bluez/out/lib/* /deps-archive/lib/ && \
    cp -r /workspace/bluez/out/include/* /deps-archive/include/ && \
    cp -r /workspace/libble/out/usr/lib/* /deps-archive/lib/ && \
    cp -r /workspace/libble/out/usr/include/* /deps-archive/include/ && \
    cp /workspace/nimble/atbm-wifi/ble_host/nimble_v42/LICENSE /deps-archive/licenses/NIMBLE-LICENSE && \
    cp /workspace/bluez/bluez-5.79/COPYING.LIB /deps-archive/licenses/BLUEZ-COPYING.LIB && \
    cp /workspace/libble/libblepp/COPYING /deps-archive/licenses/LIBBLEPP-COPYING && \
    cd /deps-archive && \
    tar -czf /workspace/deps.tar.gz lib include licenses

# Deps archive stage (for extracting dependencies)
FROM scratch AS deps
COPY --from=builder /workspace/deps.tar.gz /deps.tar.gz

# Runtime stage (optional, for smaller image)
FROM ubuntu:22.04 AS runtime

# Copy built libraries from builder
COPY --from=builder /workspace/nimble/out/ /usr/local/
COPY --from=builder /workspace/bluez/out/ /usr/local/
COPY --from=builder /workspace/libble/out/ /

# Copy built application
COPY --from=builder /workspace/build/esphome-linux /usr/local/bin/

# Update library cache
RUN ldconfig || true

EXPOSE 6053

CMD ["esphome-linux"]