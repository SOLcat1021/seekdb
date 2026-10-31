# Recall regression repair with a larger CAGRA search budget

The official evaluation of `489a611300b3cf89db4cd00610095fdfadb344be`
returned zero with `hidden250k:recall@100_baseline_delta`. This identifies
the failed gate; the platform did not supply the hidden recall values.

The rejected fast graph with 512 search candidates also regressed on public
250K-row, 256-dimensional inputs. Unit-sphere Recall@100 fell from 0.832324
to 0.817441, and clustered Recall@100 from 1.000000 to 0.989375. Both drops
exceeded the 0.005 tolerance, while the Gaussian proxy passed. One public
distribution was insufficient to establish quality on other distributions.

The conservative NN-Descent control in `a3e8143250` restored the frozen
128/256/100 graph budget and passed the expanded checks. Its weighted build
ratio was only 1.80--1.86, so it was retained as a quality fallback.
See [the control report](recall-regression-20261031.md).

## Selected policy

The fast repair keeps the bounded IVF-PQ builder and gives the installed
CAGRA search more candidates. For indexes with at least 100,000 rows and
256 dimensions, the serving budget is `max(2048, 4 * topk)`, with AUTO and
search width four. Other shapes retain the 512-candidate minimum.

The construction batch is 1,536 rows, with final/intermediate degree 128,
20 probes and 1.25x refinement. Aligned inputs up to 512 dimensions retain
four-bit scalar codewords and FP32 distance tables. Large lower-dimensional
inputs retain the independently validated 32-neighbor NN-Descent policy.

A 2,048-row construction batch exceeded the public VRAM limit (1.348x
baseline) and was rejected. The selected 1,536-row batch used about 1.118x
baseline in the six bridge diagnostics. Increasing the search candidate
budget recovered recall without increasing that measured construction peak.

The [official task, section 3.1](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/seebnbaoxot1b0tb)
permits supported CAGRA search-parameter tuning. The implementation calls
the fixed installed library; the pinned hipVS/ROCm libraries are unchanged.

## Publication acceptance

The publication build passed every applicable audit gate and all functional checks.
Its runtime bridge SHA-256 is `ae375f3bc808fa2db71d23ea60450390131935890edbb77ebf0939bca5d11a77`.
The uploaded source snapshot SHA-256 is `a853a669cb962b134d7b7d71f14dddc22f529a74beb283fddfa8e8c6e75027b4`.
All eight bridge files in that snapshot match the published source byte for byte.

| Public SQL data | Build ratio | Query ratio | Minimum candidate R@10 / R@100 | Applicable gates |
| --- | ---: | ---: | ---: | --- |
| SIFT-128 | 5.2926x | 11.0306x | 0.999902 / 0.999521 | PASS |
| NYTimes, raw L2 | 2.1475x | 7.5442x | 0.410962 / 0.531880 | PASS; relative proxy gates |
| Gaussian-256 | 2.0677x | 7.4067x | 0.999146 / 0.996990 | PASS |

| Weighted public pair | Build ratio | Query ratio | 3x / 1.5x performance thresholds |
| --- | ---: | ---: | --- |
| SIFT + nytimes | 3.0805x | 8.7823x | PASS |
| SIFT + synthetic256 | 3.0114x | 8.6859x | PASS |

| Bridge distribution | Baseline R@100 | Publication R@100 | Delta |
| --- | ---: | ---: | ---: |
| gaussian | 0.901445 | 0.996836 | +0.095391 |
| unit_sphere | 0.832324 | 0.995781 | +0.163457 |
| anisotropic | 1.000000 | 1.000000 | +0.000000 |
| clustered | 1.000000 | 1.000000 | +0.000000 |
| low_rank | 1.000000 | 1.000000 | +0.000000 |
| nytimes | 0.526797 | 0.529219 | +0.002422 |

The previous complete run of the same parameter policy measured weighted
build ratios of 3.2305x / 3.3455x. The publication repeat measured 3.0805x /
3.0114x. Performance varies between runs; meeting a public threshold does
not guarantee the official hidden-data score.

The publication source was recompiled and the resulting runtime hash was
validated again through all six distributions, all three paired SQL groups
and the functional checks. Reports use label `final-publish` and probe
version `publish`, so the earlier control measurements cannot be selected
accidentally. The audit command from the remote Notebook directory was:

```bash
python3 audit_public_recall.py \
  --work /workspace/seekdb_hipvs_notebook/batch_validation \
  --label final-publish --probe-version publish \
  --bridge /workspace/seekdb_hipvs_notebook/batch_validation/bridge-publish/libseekdb_cuvs_bridge.so
```

The bridge diagnostics use six distributions, 250,000 rows, 256 dimensions
and 512 distinct queries for each Top-k. Paired SQL uses 250,000 rows,
4,096 distinct single-row approximate-query SQL statements per Top-k,
three paired rounds, one untimed warmup, 24 CPUs and a 16 GiB observer
memory budget. The server binary is reused only because its source matches
the already compiled server; the changed bridge is compiled on AMD.

Raw NYTimes L2 is a relative-regression proxy. Both baseline and candidate
fall below its absolute contest recall thresholds, so those two proxy
absolute gates are explicitly excluded. SIFT and Gaussian must also pass
their absolute recall gates. Public results cannot establish hidden recall
or an official score.

The acceptance audit rejects false or missing applicable gates and checks
that SQL, bridge diagnostics and functional evidence name the same bridge
SHA-256. It also verifies snapshot reload, insert visibility, delete/filter
exclusion and CPU-index isolation. A process exit status alone does not
establish acceptance.
