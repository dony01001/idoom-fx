# Cross-compile environment for IDUM-FX (Ableton Move, aarch64 Linux)
FROM debian:bookworm

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    file \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
ENV CROSS_PREFIX=aarch64-linux-gnu-
