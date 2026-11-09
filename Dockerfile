FROM alpine:3.22.2@sha256:4b7ce07002c69e8f3d704a9c5d6fd3053be500b7f1c69fc0d80990c2ad8dd412 AS build
ARG ALPINE_REPOSITORY=https://dl-cdn.alpinelinux.org/alpine/v3.22/main
RUN printf '%s\n' "$ALPINE_REPOSITORY" > /etc/apk/repositories \
    && apk add --no-cache 'g++=14.2.0-r6' 'gcc=14.2.0-r6' 'libgcc=14.2.0-r6' \
        'libstdc++=14.2.0-r6' 'libstdc++-dev=14.2.0-r6' 'libgomp=14.2.0-r6' \
        'libatomic=14.2.0-r6' 'musl=1.2.5-r13' 'musl-dev=1.2.5-r13' \
        'binutils=2.44-r3' 'jansson=2.14.1-r0' 'zstd-libs=1.5.7-r0' \
        'gmp=6.3.0-r3' 'isl26=0.26-r1' 'mpfr4=4.2.1_p1-r0' 'mpc1=1.3.1-r1'
WORKDIR /build
COPY minivectordb/*.hpp minivectordb/main.cpp ./
RUN g++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -static main.cpp -o minivectordb

FROM scratch
COPY --from=build /build/minivectordb /usr/local/bin/minivectordb
WORKDIR /data
ENTRYPOINT ["/usr/local/bin/minivectordb"]
