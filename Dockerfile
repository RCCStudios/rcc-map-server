FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        libssl-dev \
        libcurl4-openssl-dev \
        libsqlite3-dev \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . /build/src

RUN cmake -S /build/src -B /build/release \
        -DCMAKE_BUILD_TYPE=Release \
        -DSSL_SUPPORT=OFF \
    && cmake --build /build/release -j"$(nproc)"

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 \
        libcurl4 \
        libsqlite3-0 \
        zlib1g \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --create-home --shell /bin/bash rcc-map-server

WORKDIR /home/rcc-map-server

COPY --from=builder /build/release/rcc-map-server /usr/local/bin/rcc-map-server
RUN mkdir -p config data web \
    && chown -R rcc-map-server:rcc-map-server /home/rcc-map-server

USER rcc-map-server

EXPOSE $SERVER_PORT

ENTRYPOINT ["rcc-map-server"]