# syntax=docker/dockerfile:1
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends cmake g++ make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY engine/ engine/
COPY shared/ shared/
RUN cmake -S engine -B build -DHFT_NATIVE=OFF && cmake --build build -j && ./build/hft-tests

FROM debian:bookworm-slim
COPY --from=build /src/build/hft-engine /src/build/hft-bench /usr/local/bin/
COPY config/ /etc/hft/
ENTRYPOINT ["hft-engine"]
