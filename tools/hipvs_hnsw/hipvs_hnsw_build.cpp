// Copyright (c) 2026 OceanBase.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cuvs/distance/distance.hpp>
#include <cuvs/neighbors/cagra.hpp>
#include <cuvs/neighbors/hnsw.hpp>
#include <raft/core/host_mdspan.hpp>
#include <raft/core/resource/cuda_stream.hpp>
#include <raft/core/resources.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct BuildOptions {
  std::string input;
  std::string output;
  int64_t rows = 0;
  int64_t dim = 0;
  int m = 32;
  int ef_construction = 200;
  cuvs::distance::DistanceType metric = cuvs::distance::DistanceType::L2Expanded;
  cuvs::neighbors::hnsw::HnswHierarchy hierarchy =
      cuvs::neighbors::hnsw::HnswHierarchy::CPU;
};

[[noreturn]] void usage(const char *reason = nullptr)
{
  if (reason != nullptr) {
    std::cerr << "error: " << reason << '\n';
  }
  std::cerr
      << "usage: seekdb_hipvs_hnsw_build --input vectors.f32 --rows N --dim D [options]\n"
      << "\n"
      << "Input is a row-major float32 matrix with exactly N * D values.\n"
      << "Options:\n"
      << "  --output FILE              Write a hipVS HNSW artifact (optional)\n"
      << "  --metric l2|ip             Distance metric (default: l2)\n"
      << "  --m N                      HNSW M (default: 32)\n"
      << "  --ef-construction N        Construction candidate count (default: 200)\n"
      << "  --hierarchy cpu|gpu|none   HNSW hierarchy mode (default: cpu)\n";
  std::exit(reason == nullptr ? EXIT_SUCCESS : EXIT_FAILURE);
}

int64_t parse_positive_i64(const char *value, const char *name)
{
  try {
    size_t parsed = 0;
    const int64_t result = std::stoll(value, &parsed);
    if (value[parsed] != '\0' || result <= 0) {
      usage(name);
    }
    return result;
  } catch (const std::exception &) {
    usage(name);
  }
}

int parse_positive_int(const char *value, const char *name)
{
  const int64_t parsed = parse_positive_i64(value, name);
  if (parsed > std::numeric_limits<int>::max()) {
    usage(name);
  }
  return static_cast<int>(parsed);
}

BuildOptions parse_options(int argc, char **argv)
{
  BuildOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument(argv[i]);
    if (argument == "--help") {
      usage();
    }
    if (i + 1 == argc) {
      usage("option requires a value");
    }
    const char *value = argv[++i];
    if (argument == "--input") {
      options.input = value;
    } else if (argument == "--output") {
      options.output = value;
    } else if (argument == "--rows") {
      options.rows = parse_positive_i64(value, "--rows must be a positive integer");
    } else if (argument == "--dim") {
      options.dim = parse_positive_i64(value, "--dim must be a positive integer");
    } else if (argument == "--m") {
      options.m = parse_positive_int(value, "--m must be a positive integer");
    } else if (argument == "--ef-construction") {
      options.ef_construction = parse_positive_int(
          value, "--ef-construction must be a positive integer");
    } else if (argument == "--metric") {
      if (std::string(value) == "l2") {
        options.metric = cuvs::distance::DistanceType::L2Expanded;
      } else if (std::string(value) == "ip") {
        options.metric = cuvs::distance::DistanceType::InnerProduct;
      } else {
        usage("--metric must be l2 or ip");
      }
    } else if (argument == "--hierarchy") {
      if (std::string(value) == "cpu") {
        options.hierarchy = cuvs::neighbors::hnsw::HnswHierarchy::CPU;
      } else if (std::string(value) == "gpu") {
        options.hierarchy = cuvs::neighbors::hnsw::HnswHierarchy::GPU;
      } else if (std::string(value) == "none") {
        options.hierarchy = cuvs::neighbors::hnsw::HnswHierarchy::NONE;
      } else {
        usage("--hierarchy must be cpu, gpu, or none");
      }
    } else {
      usage("unknown option");
    }
  }
  if (options.input.empty() || options.rows == 0 || options.dim == 0) {
    usage("--input, --rows, and --dim are required");
  }
  if (options.rows > std::numeric_limits<int64_t>::max() / options.dim ||
      static_cast<uint64_t>(options.rows) * static_cast<uint64_t>(options.dim) >
          std::numeric_limits<size_t>::max() / sizeof(float)) {
    usage("matrix dimensions overflow addressable input size");
  }
  return options;
}

std::vector<float> read_vectors(const BuildOptions &options)
{
  const size_t count = static_cast<size_t>(options.rows * options.dim);
  const size_t byte_count = count * sizeof(float);
  if (byte_count > static_cast<size_t>(std::numeric_limits<std::streamoff>::max())) {
    throw std::runtime_error("input matrix is too large for this platform");
  }
  std::ifstream input(options.input, std::ios::binary | std::ios::ate);
  if (!input) {
    throw std::runtime_error("cannot open input file: " + options.input);
  }
  const std::streamoff expected_bytes = static_cast<std::streamoff>(byte_count);
  if (input.tellg() != expected_bytes) {
    throw std::runtime_error("input size does not equal rows * dim * sizeof(float)");
  }
  std::vector<float> vectors(count);
  input.seekg(0);
  input.read(reinterpret_cast<char *>(vectors.data()), expected_bytes);
  if (!input) {
    throw std::runtime_error("cannot read complete input matrix");
  }
  return vectors;
}

}  // namespace

int main(int argc, char **argv)
{
  try {
    const BuildOptions options = parse_options(argc, argv);
    std::vector<float> vectors = read_vectors(options);
    const auto dataset = raft::make_host_matrix_view<const float, int64_t>(
        vectors.data(), options.rows, options.dim);

    raft::resources resources;
    auto cagra_params = cuvs::neighbors::hnsw::to_cagra_params(
        dataset.extents(), options.m, options.ef_construction, options.metric);
    cagra_params.metric = options.metric;
    const auto started = std::chrono::steady_clock::now();
    auto cagra_index = cuvs::neighbors::cagra::build(resources, cagra_params, dataset);

    cuvs::neighbors::hnsw::index_params hnsw_params;
    hnsw_params.hierarchy = options.hierarchy;
    hnsw_params.ef_construction = options.ef_construction;
    auto hnsw_index = cuvs::neighbors::hnsw::from_cagra(
        resources, hnsw_params, cagra_index, dataset);
    raft::resource::sync_stream(resources);
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();

    if (!options.output.empty()) {
      cuvs::neighbors::hnsw::serialize(resources, options.output, *hnsw_index);
    }
    std::cout << "hipVS HNSW build completed: rows=" << options.rows
              << " dim=" << options.dim << " seconds=" << elapsed << '\n';
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "hipVS HNSW build failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
