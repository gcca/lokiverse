ARG WOLFI_BASE=cgr.dev/chainguard/wolfi-base:latest@sha256:94fa13e26d449f7d78dbcd02d12ea37a0da80d24fcc3e1f17b95d515dec607a7

FROM ${WOLFI_BASE} AS build
RUN apk add --no-cache build-base cmake ninja-build \
      'protobuf-dev~36.2' 'protoc~36.2' 'protobuf-static~36.2' abseil-cpp-static \
      'snappy-dev~1.3.1' 'snappy-static~1.3.1' 'gtest-dev~1.18.0' \
 && ln -s /usr/lib/ninja-build/bin/ninja /usr/local/bin/ninja
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake cmake
COPY src src
COPY proto proto
COPY tests tests
COPY 3rd 3rd
RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DCMAKE_INSTALL_LIBDIR=lib \
      -DLOKIVERSE_BUILD_TESTS=ON \
      -DLOKIVERSE_BUILD_PYTHON=OFF \
      -DLOKIVERSE_INSTALL=ON \
      -DLOKIVERSE_STATIC_DEPS=ON \
 && cmake --build build \
 && ctest --test-dir build --output-on-failure \
 && DESTDIR=/stage cmake --install build

FROM ${WOLFI_BASE} AS dev
RUN apk add --no-cache build-base cmake ninja-build \
      'protobuf-dev~36.2' 'protoc~36.2' 'protobuf-static~36.2' abseil-cpp-static \
      'snappy-dev~1.3.1' 'snappy-static~1.3.1' \
 && ln -s /usr/lib/ninja-build/bin/ninja /usr/local/bin/ninja
COPY --from=build /stage/usr/local /usr/local
LABEL org.opencontainers.image.title="lokiverse-dev" \
      org.opencontainers.image.description="Wolfi build image with lokiverse and static Protobuf, Abseil and Snappy installed" \
      org.opencontainers.image.licenses="AGPL-3.0-only"
