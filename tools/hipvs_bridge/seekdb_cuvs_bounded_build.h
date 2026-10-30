#pragma once
#include <cuvs/neighbors/cagra.h>

#ifdef __cplusplus
extern "C" {
#endif
cuvsError_t sk_cuvs_cagra_build_bounded(cuvsResources_t resources,
                                      cuvsCagraIndexParams_t params,
                                      DLManagedTensor *dataset,
                                      cuvsCagraIndex_t index);
#ifdef __cplusplus
}
#endif
