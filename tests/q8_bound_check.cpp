#include "quantized_scan.h"
#include <cassert>
#include <iostream>
#include <random>

// Independent long-double reference. No exact_scan or engine distance helper.
long double reference(const float* a, const float* b, size_t n) {
    long double sum = 0;
    for (size_t i=0; i<n; ++i) {
        long double d = (long double)a[i] - (long double)b[i];
        sum += d*d;
    }
    return std::sqrt(sum);
}
int main() {
    std::mt19937 rng(20261112);
    std::uniform_real_distribution<float> uniform(-1,1);
    uint64_t checks=0, rejected=0, vectors=0, fallbacks=0;
    bool avx=false;
#if defined(__x86_64__) || defined(__i386__)
    avx=__builtin_cpu_supports("avx2");
#endif
    for (size_t dim : {1,2,15,16,17,31,128,129,4096}) {
        for (int exponent : {-149,-140,-100,-20,0,25,80,120}) {
            std::vector<float> data(129*dim), q(dim);
            float scale=std::ldexp(1.0f,exponent);
            for (float& x:data) x=uniform(rng)*scale;
            // Exact matches, ties, near-equal large values and mixed magnitudes.
            for (size_t k=0;k<4;++k) {
                if (k==1) for (float& x:data) x=std::nextafter(scale, 0.0f);
                if (k==2) for (size_t i=0;i<data.size();++i) data[i]=(i%2)?scale:std::ldexp(scale,-20);
                if (k==3) for (size_t i=0;i<data.size();++i) data[i]=std::round(uniform(rng)*126)*scale/128;
                quantized_scan::Cache cache; cache.build(data,dim);
                assert(cache.codes.size()==data.size());
                for (size_t qi=0;qi<12;++qi) {
                    if (qi%3==0) std::copy_n(data.data()+(qi%129)*dim,dim,q.data());
                    else for (float& x:q) x=uniform(rng)*scale;
                    quantized_scan::Query screen(cache,q,avx,true);
                    if (!screen.cache) { ++fallbacks; continue; }
                    for (size_t row=0;row<129;++row) {
                        ++vectors;
                        auto sq=quantized_scan::scalar(cache.codes.data()+row*dim,screen.codes.data(),dim);
#if defined(__x86_64__) || defined(__i386__)
                        if(avx) assert(sq==quantized_scan::avx2(cache.codes.data()+row*dim,screen.codes.data(),dim));
#endif
                        long double actual=reference(data.data()+row*dim,q.data(),dim);
                        double d=double(actual);
                        for (double threshold : {0.0,d*0.1,d*0.9,std::nextafter(d,0.0),d,std::nextafter(d,INFINITY),d*1.1}) {
                            bool drop=screen.rejects(row,threshold);
                            ++checks; rejected+=drop;
                            if(drop && !(actual>(long double)threshold)) {
                                std::cerr<<"unsafe prune dim="<<dim<<" exponent="<<exponent<<" row="<<row<<"\n";
                                return 1;
                            }
                        }
                    }
                }
                q.assign(dim,float(cache.scale*256));
                quantized_scan::Query outside(cache,q,avx,true);
                assert(!outside.cache);
                quantized_scan::Query descending(cache,q,avx,false);
                assert(!descending.cache);
            }
        }
    }
    quantized_scan::Cache oversized;
    oversized.build(std::vector<float>(4097,1),4097); assert(!oversized.scale);
    quantized_scan::Cache invalid;
    invalid.build({1,INFINITY},2); assert(!invalid.scale);
    std::cout<<"{\"bound_checks\":"<<checks<<",\"vector_pairs\":"<<vectors
             <<",\"rejected\":"<<rejected<<",\"out_of_range_query_fallbacks\":"<<fallbacks
             <<",\"unsafe_prunes\":0,\"avx2\":"<<(avx?"true":"false")<<"}\n";
}
