add_executable(seekdb_hipvs_hnsw_build hipvs_hnsw_build.cpp)

# hnsw.hpp hides the CAGRA-to-HNSW API unless hipVS was built with hnswlib
# support. Defining this makes a missing capability a build error instead of
# silently omitting the requested path.
target_compile_definitions(seekdb_hipvs_hnsw_build PRIVATE CUVS_BUILD_CAGRA_HNSWLIB)
target_compile_features(seekdb_hipvs_hnsw_build PRIVATE cxx_std_17)
target_link_libraries(seekdb_hipvs_hnsw_build PRIVATE cuvs::cuvs)
