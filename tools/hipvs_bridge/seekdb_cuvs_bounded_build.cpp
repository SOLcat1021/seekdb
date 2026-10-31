// Build IVF-PQ CAGRA graphs with a bounded temporary query batch.
#include "seekdb_cuvs_bounded_build.h"
#include <cuvs/neighbors/cagra.hpp>
#include <raft/core/host_mdspan.hpp>
#include <raft/core/resources.hpp>
#include <cstdint>
#include <exception>
#include <memory>

#ifndef SEEKDB_CUVS_BUILD_BATCH
#define SEEKDB_CUVS_BUILD_BATCH 1536
#endif
#ifndef SEEKDB_CUVS_BUILD_REFINEMENT
#define SEEKDB_CUVS_BUILD_REFINEMENT 1.25
#endif

extern "C" cuvsError_t sk_cuvs_cagra_build_bounded(
    cuvsResources_t resources, cuvsCagraIndexParams_t params,
    DLManagedTensor *dataset, cuvsCagraIndex_t index)
{
  // Keep the existing C path for NN-Descent and all other algorithms.
  if (params != nullptr && params->build_algo != IVF_PQ) {
    return cuvsCagraBuild(resources, params, dataset, index);
  }
  if (resources == 0 || params == nullptr || dataset == nullptr || index == nullptr) {
    cuvsSetLastErrorText("bounded CAGRA build: null input");
    return CUVS_ERROR;
  }
  const auto &tensor = dataset->dl_tensor;
  if (tensor.device.device_type != kDLCPU || tensor.ndim != 2 ||
      tensor.shape == nullptr || tensor.shape[0] <= 1 || tensor.shape[1] <= 0 ||
      tensor.data == nullptr || tensor.dtype.code != kDLFloat ||
      tensor.dtype.bits != 32 || tensor.dtype.lanes != 1 ||
      tensor.strides != nullptr || params->compression != nullptr || index->addr != 0) {
    cuvsSetLastErrorText("bounded CAGRA build requires a fresh, contiguous host FP32 index");
    return CUVS_ERROR;
  }
  try {
    auto &res = *reinterpret_cast<raft::resources *>(resources);
    const auto *data = reinterpret_cast<const float *>(
        static_cast<const unsigned char *>(tensor.data) + tensor.byte_offset);
    auto vectors = raft::make_host_matrix_view(data, tensor.shape[0], tensor.shape[1]);
    cuvs::neighbors::cagra::index_params cpp_params;
    cpp_params.metric = static_cast<cuvs::distance::DistanceType>(params->metric);
    cpp_params.graph_degree = params->graph_degree;
    cpp_params.intermediate_graph_degree = params->intermediate_graph_degree;
    auto graph_params = cuvs::neighbors::graph_build_params::ivf_pq_params(
        vectors.extents(), cpp_params.metric);
    // The fixed hipVS C adapter drops the batch bound; use its public C++ API
    // to control temporary memory and graph quality. The installed hipVS libraries
    // stay unchanged; serving uses their supported CAGRA search parameters.
    graph_params.search_params.max_internal_batch_size = SEEKDB_CUVS_BUILD_BATCH;
    graph_params.search_params.n_probes = 20;
    // Use scalar codewords for aligned vectors up to 512 dimensions; retain
    // the hipVS default layout and precision for other shapes.
    if (tensor.shape[1] % 32 == 0 && tensor.shape[1] <= 512) {
      graph_params.build_params.pq_dim = static_cast<uint32_t>(tensor.shape[1]);
      graph_params.build_params.pq_bits = 4;
      graph_params.search_params.lut_dtype = CUDA_R_32F;
      graph_params.search_params.internal_distance_dtype = CUDA_R_32F;
      graph_params.search_params.coarse_search_dtype = CUDA_R_32F;
    }
    graph_params.refinement_rate = SEEKDB_CUVS_BUILD_REFINEMENT;
    cpp_params.graph_build_params = graph_params;
    using Graph = cuvs::neighbors::cagra::index<float, uint32_t>;
    auto graph = std::make_unique<Graph>(res);
    *graph = cuvs::neighbors::cagra::build(res, cpp_params, vectors);
    index->dtype = tensor.dtype;
    index->addr = reinterpret_cast<uintptr_t>(graph.release());
    return CUVS_SUCCESS;
  } catch (const std::exception &error) {
    cuvsSetLastErrorText(error.what());
    return CUVS_ERROR;
  } catch (...) {
    cuvsSetLastErrorText("bounded CAGRA build: unknown exception");
    return CUVS_ERROR;
  }
}
