# Variant for the competition submission repository.
#
# Copy this file to the repository root as `Dockerfile`, and copy the C++
# project into `minivector-cpp/` (Makefile, src/).  The build context is the
# repository root.

FROM gcc:13-bookworm AS builder
WORKDIR /build
COPY minivector-cpp/Makefile ./
COPY minivector-cpp/src ./src
RUN make

FROM debian:bookworm-slim
RUN mkdir -p /data
COPY --from=builder /build/bin/minivector /usr/local/bin/minivector
WORKDIR /
ENTRYPOINT ["/usr/local/bin/minivector", "--data", "/data"]
