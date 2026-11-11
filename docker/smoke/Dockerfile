ARG BASE=lokiverse-dev
ARG STATIC=cgr.dev/chainguard/static:latest@sha256:fc42af33e643f20d08baf03cddcc1c02ba57052dfb02edb0d79f3fc47bcea8d9

FROM ${BASE} AS build
WORKDIR /src
COPY CMakeLists.txt main.cpp ./
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build \
 && readelf -d build/smoke | tee /tmp/dynamic \
 && ! grep -q NEEDED /tmp/dynamic

FROM ${STATIC}
COPY --from=build /src/build/smoke /smoke
ENTRYPOINT ["/smoke"]
