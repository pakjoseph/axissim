FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf \
    automake \
    bison \
    build-essential \
    ca-certificates \
    clang \
    cmake \
    curl \
    flex \
    gawk \
    gperf \
    help2man \
    git \
    libelf-dev \
    libexpat1-dev \
    libfl-dev \
    libgmp-dev \
    libisl-dev \
    libmpc-dev \
    libmpfr-dev \
    libtool \
    make \
    ninja-build \
    pkg-config \
    python3 \
    python3-numpy \
    texinfo \
    wget \
    zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
