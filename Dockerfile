# This is a multi-stage build.
# The first stage installs the build time libraries and compiles the application
# The second stage installs the runtime libraries only and copies the compiled binary from the previous stage.
# This dockerfile assumes it's run in the context of `https://github.com/omardoescode/KV-Store`

# Instead of cloning the repo, you can run these commands
# Build: docker build -t kv-store https://github.com/omardoescode/KV-Store.git
# Run:   docker run -p 7890:7890 kv-store

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
