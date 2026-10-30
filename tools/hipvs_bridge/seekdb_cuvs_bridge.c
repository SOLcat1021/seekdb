/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <pthread.h>

/* Keep the final graph degree unchanged while reducing the temporary NN-Descent
 * pool and rounds.  The environment overrides make recall/speed sweeps possible
 * on the AMD host without rebuilding the bridge. */
#define SK_DEFAULT_GRAPH_DEGREE 128
#define SK_DEFAULT_INTERMEDIATE_DEGREE 128
#define SK_DEFAULT_NN_DESCENT_NITER 64

static int sk_positive_env(const char *name, int fallback)
{
  const char *text = getenv(name);
  if (text == NULL || *text == '\0') return fallback;
  errno = 0;
  char *end = NULL;
  long value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value <= 0 || value > INT_MAX) {
    return fallback;
  }
  return (int)value;
}

static void sk_apply_build_params(cuvsCagraIndexParams_t params)
{
  params->build_algo = NN_DESCENT;
  params->graph_degree = sk_positive_env("SEEKDB_CUVS_GRAPH_DEGREE", SK_DEFAULT_GRAPH_DEGREE);
  params->intermediate_graph_degree = sk_positive_env(
      "SEEKDB_CUVS_INTERMEDIATE_DEGREE", SK_DEFAULT_INTERMEDIATE_DEGREE);
  params->nn_descent_niter = sk_positive_env(
      "SEEKDB_CUVS_NN_DESCENT_NITER", SK_DEFAULT_NN_DESCENT_NITER);
}

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  /* Search is serialized per handle because cuVS resources and the reusable
   * device buffers below are not safe to mutate concurrently.  Reusing these
   * allocations removes three RMM alloc/free pairs and a params create/destroy
   * pair from every query. */
  pthread_mutex_t         search_mu;
  int                     search_mu_ready;
  cuvsCagraSearchParams_t search_params;
  size_t                  search_default_itopk;
  float                  *query_device;
  unsigned               *neighbors_device;
  float                  *distances_device;
  size_t                  query_capacity;
  size_t                  neighbors_capacity;
  size_t                  distances_capacity;
} sk_cuvs_index;

static int sk_cuvs_2d_bytes(long rows, long cols, size_t elem, size_t *bytes)
{
  if (rows <= 0 || cols <= 0 || elem == 0 || bytes == NULL) return 0;
  const uint64_t r = (uint64_t)rows;
  const uint64_t c = (uint64_t)cols;
  if (r > (uint64_t)SIZE_MAX / c) return 0;
  const uint64_t count = r * c;
  if (count > (uint64_t)SIZE_MAX / elem) return 0;
  *bytes = (size_t)(count * elem);
  return 1;
}

static int sk_cuvs_ensure_buffer(cuvsResources_t res, void **ptr,
                                 size_t *capacity, size_t bytes)
{
  if (ptr == NULL || capacity == NULL || bytes == 0) return 0;
  if (*ptr != NULL && *capacity >= bytes) return 1;
  void *next = NULL;
  if (cuvsRMMAlloc(res, &next, bytes) != CUVS_SUCCESS) return 0;
  if (*ptr != NULL) {
    (void)cuvsRMMFree(res, *ptr, *capacity);
  }
  *ptr = next;
  *capacity = bytes;
  return 1;
}

static void sk_cuvs_release_search_state(sk_cuvs_index *h)
{
  if (h == NULL) return;
  if (h->search_params != NULL) {
    (void)cuvsCagraSearchParamsDestroy(h->search_params);
    h->search_params = NULL;
  }
  if (h->distances_device != NULL) {
    (void)cuvsRMMFree(h->res, h->distances_device, h->distances_capacity);
    h->distances_device = NULL;
    h->distances_capacity = 0;
  }
  if (h->neighbors_device != NULL) {
    (void)cuvsRMMFree(h->res, h->neighbors_device, h->neighbors_capacity);
    h->neighbors_device = NULL;
    h->neighbors_capacity = 0;
  }
  if (h->query_device != NULL) {
    (void)cuvsRMMFree(h->res, h->query_device, h->query_capacity);
    h->query_device = NULL;
    h->query_capacity = 0;
  }
  h->search_default_itopk = 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  sk_apply_build_params(h->ip);
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  if (pthread_mutex_init(&h->search_mu, NULL) != 0) goto fail_idx;
  h->search_mu_ready = 1;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !query || !out_ids || nq <= 0 || topk <= 0 || h->dim <= 0 ||
      !h->search_mu_ready) return 1;
  if (pthread_mutex_lock(&h->search_mu) != 0) return 2;
  int rc = 0;
  size_t query_bytes = 0, result_bytes = 0, distance_bytes = 0;
  if (!sk_cuvs_2d_bytes(nq, h->dim, sizeof(float), &query_bytes) ||
      !sk_cuvs_2d_bytes(nq, topk, sizeof(unsigned), &result_bytes) ||
      !sk_cuvs_2d_bytes(nq, topk, sizeof(float), &distance_bytes)) {
    rc = 3;
    goto done;
  }
  if (!sk_cuvs_ensure_buffer(h->res, (void**)&h->query_device,
                            &h->query_capacity, query_bytes)) { rc=10; goto done; }
  if (!sk_cuvs_ensure_buffer(h->res, (void**)&h->neighbors_device,
                            &h->neighbors_capacity, result_bytes)) { rc=11; goto done; }
  if (!sk_cuvs_ensure_buffer(h->res, (void**)&h->distances_device,
                            &h->distances_capacity, distance_bytes)) {
    rc=12; goto done;
  }
  if (cudaMemcpy(h->query_device, query, query_bytes, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=h->query_device; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=h->neighbors_device; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=h->distances_device; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (h->search_params == NULL) {
      if (cuvsCagraSearchParamsCreate(&h->search_params) != CUVS_SUCCESS) { rc=14; goto done; }
      h->search_default_itopk = h->search_params->itopk_size;
    }
    {
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 512) required_itopk = 512;
      h->search_params->itopk_size = h->search_default_itopk > required_itopk
          ? h->search_default_itopk : required_itopk;
      h->search_params->search_width = 4;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->search_params, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, h->neighbors_device, result_bytes, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist && cudaMemcpy(out_dist, h->distances_device, distance_bytes,
                               cudaMemcpyDefault) != cudaSuccess) { rc=17; goto done; }
  }
done:
  (void)pthread_mutex_unlock(&h->search_mu);
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  if (h->search_mu_ready) {
    (void)pthread_mutex_lock(&h->search_mu);
    sk_cuvs_release_search_state(h);
    (void)pthread_mutex_unlock(&h->search_mu);
    (void)pthread_mutex_destroy(&h->search_mu);
    h->search_mu_ready = 0;
  }
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  if (pthread_mutex_init(&index->search_mu, NULL) != 0) goto fail_idx;
  index->search_mu_ready = 1;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
