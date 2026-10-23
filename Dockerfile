FROM debian:bookworm AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       gcc g++ make cmake pkg-config libssl-dev zlib1g-dev libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile ./
COPY third_party ./third_party
COPY src ./src
RUN make -j"$(nproc)" build/app

FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends libssl3 zlib1g libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --no-create-home kv \
    && mkdir /data \
    && chown kv:kv /data

COPY --from=build /src/build/app /usr/local/bin/kv-store

ENV KV_LISTEN_ADDR=0.0.0.0:7890 \
    KV_LOG_PATH=- \
    KV_DB_PATH=/data/kv.db

USER kv
VOLUME /data
EXPOSE 7890
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/kv-store"]
