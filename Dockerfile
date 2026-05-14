# =============================================================================
# ConcurrentCache Dockerfile
# A high-performance C++ in-memory cache system compatible with Redis RESP
# =============================================================================

# -----------------------------------------------------------------------------
# Stage 1: Build Stage
# -----------------------------------------------------------------------------
FROM gcc:14 AS builder

WORKDIR /build

# Install build dependencies
RUN apt-get update && apt-get install -y \
    cmake \
    ninja-build \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy source code
COPY . /build

# Build ConcurrentCache
RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel

# -----------------------------------------------------------------------------
# Stage 2: Runtime Stage
# -----------------------------------------------------------------------------
FROM debian:bookworm-slim

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    zlib1g \
    ca-certificates \
    redis-tools \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -m -s /bin/bash appuser

WORKDIR /app

# Copy binary from builder
COPY --from=builder /build/build/concurrentcache-server /app/
COPY --from=builder /build/conf /app/conf

# Create data directory
RUN mkdir -p /app/data && chown -R appuser:appuser /app

# Switch to non-root user
USER appuser

# Expose default Redis port
EXPOSE 6379

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD redis-cli -p 6379 PING > /dev/null 2>&1 || exit 1

# Run ConcurrentCache
ENTRYPOINT ["/app/concurrentcache-server"]
CMD ["--config", "/app/conf/concurrentcache.conf"]
