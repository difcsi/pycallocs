FROM ubuntu:20.04

RUN apt-get update -y
RUN DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get install -y \
build-essential libbsd-dev libelf-dev libdw-dev binutils-dev \
autoconf automake libtool pkg-config autoconf-archive \
g++ ocaml ocamlbuild ocaml-findlib libnum-ocaml-dev \
default-jdk-headless python3 \
make git gawk gdb wget \
libunwind-dev libc6-dev-i386 zlib1g-dev libc6-dbg \
libboost-iostreams-dev libboost-regex-dev \
libboost-serialization-dev libboost-filesystem-dev libffi-dev