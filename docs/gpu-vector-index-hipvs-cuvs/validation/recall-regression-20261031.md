# Baseline-relative Recall@100 regression

The official evaluation of `489a611300b3cf89db4cd00610095fdfadb344be`
returned zero with `hidden250k:recall@100_baseline_delta`. No hidden recall
values were supplied. Passing public proxy checks did not establish that
the quantized IVF-PQ graph policy preserved recall on other distributions.

The repair restores NN-Descent with final degree 128, intermediate degree
256 and 100 iterations for high-dimensional inputs. These are the frozen
V5 graph-quality settings. Query parameters stay AUTO, at least 512 internal
candidates, multiplier four and search width four. Buffer and worker reuse
remain enabled. The large low-dimensional policy is unchanged.

The C API factory defaults to SINGLE_CTA; the previous candidate's explicit
AUTO setting is retained in this control run. They are different search
implementations within the fixed library. The [official task, section 3.1](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/seebnbaoxot1b0tb)
permits supported search-parameter tuning, so comparisons must measure the
resulting recall rather than assume that these settings are equivalent.

## Additional distribution checks

Each bridge run used 250,000 rows, 256 dimensions and 512 distinct queries
for each Top-k. Recall is measured against exact L2 neighbors, with a maximum
allowed decrease of 0.005 from frozen V5. This is a GPU bridge diagnostic,
separate from the 4,096-query paired SQL protocol.

| Distribution | V5 Recall@100 | Rejected V8 | NN repair |
| --- | ---: | ---: | ---: |
| Gaussian | 0.901445 | 0.905391 | 0.906270 |
| Unit sphere | 0.832324 | 0.817441 | 0.843105 |
| Anisotropic | 1.000000 | 1.000000 | 1.000000 |
| Clustered | 1.000000 | 0.989375 | 0.996445 |
| Low rank | 1.000000 | 1.000000 | 1.000000 |
| NYTimes, raw L2 | 0.526797 | 0.529961 | 0.528418 |

V8 reproduces a relative Recall@100 failure on the unit-sphere and clustered
proxies. The NN repair passes both relative recall gates on all six proxies
and returns distinct, in-range IDs. Its worst Recall@100 decrease in this
run is 0.003555. Repeating the build can produce different graphs, so these
are observed results rather than deterministic accuracy guarantees.

The unit-sphere and raw NYTimes baselines themselves fall below some absolute
contest recall thresholds. Those distributions test relative regression;
they do not establish the absolute recall of the official hidden data.

Returning only to the older 128/128/64 NN-Descent budget was insufficient:
on the Gaussian proxy its Recall@100 decrease was 0.009395. That candidate
was rejected before submission.

## End-to-end validation

Paired SQL validation used 250,000 rows, 4,096 distinct single-row queries
per Top-k, three paired rounds and one untimed warmup per version.

| Public data | Build ratio | Query ratio | Applicable gates |
| --- | ---: | ---: | --- |
| SIFT-128 | 4.7887 | 10.8202 | PASS |
| NYTimes, raw L2 | 0.9937 | 8.2683 | PASS, excluding the two proxy absolute-recall gates |
| Gaussian-256 | 0.9361 | 8.8409 | PASS |

NYTimes Recall@100 minima were 0.531042 for V5 and 0.531057 for the repair.
Gaussian-256 minima were 0.899336 and 0.904324. SIFT minima were 0.999971 and
0.999514. ID validity, relative recall, query throughput and measured VRAM
checks passed. SIFT and Gaussian also passed their absolute recall checks.

The saved GPU graphs reloaded after observer restart without rebuilding;
filtered/deleted IDs were excluded, inserted data was visible, and VSAG CPU
indexes stayed isolated. All functional checks passed. SQL, distribution and
functional evidence refer to bridge SHA-256
`c6071de12ff73bbfd4ba767359ae7edbd80a1821377ba9c6a5c23d6ed78f88a3`.

`audit_public_recall.py` checked all applicable gates and those bridge hashes
with no failures. The weighted public build ratio is only 1.80--1.86, below
the 3x full-performance threshold, so this is a conservative quality repair.
No new official hidden-data result has been obtained for it.
