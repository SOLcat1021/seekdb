# hipVS HNSW construction tool

`seekdb_hipvs_hnsw_build` is an opt-in ROCm/Linux developer tool for building an
HNSW index from a contiguous float32 matrix with AMD hipVS. It uses hipVS CAGRA
to construct the base graph, then converts it with `hnsw::from_cagra`.

It is deliberately not linked into the normal seekdb server build. seekdb's
production vector-index implementation is backed by vsag, which owns the
on-disk format, incremental updates, filters, and query path. A hipVS artifact
is not a valid vsag serialization, so treating one as the other would make
existing indexes unreadable. This tool provides a reproducible GPU build path
and a measured integration boundary while a format-compatible importer is
developed.

## Build

Install ROCm and hipVS with HNSW support on a Linux AMD GPU host. Configure
seekdb with a C++ compiler supported by that hipVS installation (normally
`hipcc`), then enable the optional target:

```bash
cmake -S . -B build-hipvs -DOB_ENABLE_HIPVS_HNSW=ON -DCMAKE_CXX_COMPILER=hipcc
cmake --build build-hipvs --target seekdb_hipvs_hnsw_build -j
```

The normal build leaves this target out and does not search for ROCm or hipVS.

## Run

The input must contain exactly `rows * dim` little-endian float32 values in
row-major order. `--hierarchy cpu` is the default because hipVS documents that
its serialized output is compatible with original hnswlib; `none` is useful for
in-memory experiments only.

```bash
./build-hipvs/tools/hipvs_hnsw/seekdb_hipvs_hnsw_build \
  --input corpus.f32 --rows 1000000 --dim 768 --metric l2 \
  --m 32 --ef-construction 200 --hierarchy cpu --output corpus.hnsw
```

The printed duration covers CAGRA graph construction and CAGRA-to-HNSW
conversion. Compare it with the same corpus and HNSW parameters in seekdb's
existing vsag construction path; record the hardware, ROCm/hipVS versions, and
both timings when reporting acceleration.
