# Linux profiling image: build dynajs (io_uring + native modules), plus wrk,
# node, and linux-perf, for (1) io_uring buffer-pool tuning experiments and
# (2) perf-profiling the interpreter under JetStream.
#   docker build --platform linux/arm64 -f docker/Dockerfile.profile -t dyna:profile .
#   docker run --rm --platform linux/arm64 --privileged dyna:profile <script>
FROM node:26-trixie-slim
# essential build deps (must succeed -- trixie liburing has io_uring_setup_buf_ring)
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      clang lld make libc6-dev liburing-dev curl ca-certificates procps \
 && rm -rf /var/lib/apt/lists/*
# load-gen + profiler (best-effort)
RUN apt-get update && apt-get install -y --no-install-recommends wrk linux-perf; \
    rm -rf /var/lib/apt/lists/*; true
WORKDIR /src
COPY . .
RUN make clean || true \
 && make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y CONFIG_IO_URING=y -j"$(nproc)" dynajs \
 && cp dynajs dynajs-uring \
 && make clean || true \
 && make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y -j"$(nproc)" dynajs \
 && cp dynajs dynajs-epoll
CMD ["bash", "/src/docker/profile-run.sh"]
