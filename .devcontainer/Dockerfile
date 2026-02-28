# Based off https://github.com/hdl/containers/blob/main/debian-bullseye/nvc/Dockerfile
FROM ubuntu:24.04

ARG USERNAME=ubuntu
ARG USERHOME=/home/$USERNAME

ENV LLVM_VERSION=20

ARG DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    LC_CTYPE=C.UTF-8

# Install neccessary build tools and dependencies.
RUN <<EOF
    set -e
    apt-get -q -y update
    apt-get -q -y install \
        git \
        wget \
        gpg \
        automake \
        autoconf \
        check \
        flex \
        bison \
        libdw-dev \
        libffi-dev \
        llvm-$LLVM_VERSION-dev \
        pkg-config \
        zlib1g-dev \
        libzstd-dev \
        tcl-dev \
        libreadline-dev \
        gdb \
        gdbserver
    apt-get clean
    rm -rf /var/lib/apt/lists/*
EOF

USER $USERNAME
WORKDIR $USERHOME
