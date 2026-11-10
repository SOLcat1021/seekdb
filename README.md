# MiniVectorDB：C++ 精确查询优化候选

当前根 Dockerfile 使用 GCC13 构建 `minivector-cpp/`，运行入口为 `/usr/local/bin/minivector --data /data`，通过正式 JSONL 协议通信。发布源码固定自工作候选 `9484259f3a8f94ee778d33c126a7dc054a981607`：直接差平方安全筛选、补偿 double 精算、按距离和主键排序，以及 JSON/SQL 解析优化；本次不继续修改引擎算法。

`agent/conversation.json` 按用户要求提交空对象 `{}`，不含学习证据。它与源码、Dockerfile 一起纳入本次提交。

构建：`docker build -t minivectordb:release .`。验证结果及限制见 `docs/发布验收_20261110.md`；公开测试通过不等于正式评测通过。

以下为旧 Python 原型的历史说明。旧文件保留供追溯，根 Dockerfile 不构建或运行它们，旧测试结果不作为本次 C++ 候选的验收证据。

## 旧 Python 原型说明（历史）

实现规划见 [PLAN.md](PLAN.md)。当前程序自行实现了限定 SQL 的解析与执行、精确 L2 检索、事务回滚、带校验的提交日志，以及二进制磁盘记录和内存主键哈希桶目录。数据库保存在 `--data-dir` 指定目录。查询优化包括主键等值条件直接定位、顺序扫描跳过旧版本、受限向量缓存，以及自研 C++ 精确 TopK 扫描。写入路径复用已验证的变更，WAL 大帧采用压缩格式，并兼容旧的未压缩帧；不再写入重启时从不读取的 `.idx` 文件。原生库缺失或缓存不足时自动回退到 Python 精确扫描。`CREATE VECTOR INDEX` 目前只验证语法，尚未建立近似索引；ANN 公开样例由精确检索满足。

当前已同步官方 `2026-competition-round2` 分支的公开题包 v4：20 个样例、95 条 SQL。`make test` 通过 API 与前台进程各跑一次全部样例，并检查事务恢复、向量检索与超内存数据；Docker 镜像也已构建，并通过容器入口跑完全部 95 条公开 SQL。公开样例通过不等于正式 29 个用例全部通过。正式 stdin/stdout 协议驱动、错误码映射与模式切换方式尚未包含在此公开仓库中，因此现有逐行 SQL／JSON 输出仍是**本地临时协议**，不能宣称官方环境预检已通过。

在 Linux x86_64 上运行：

```bash
make test
make probe
make probe-cgroup
make scan
python3 minivectordb.py --data-dir /tmp/mvd-demo
```

`make test` 编译 `native_scan.cpp` 为 `libmvdscan.so`，运行公开用例、进程接口、恢复、随机 TopK 对照和超内存数据测试。需要 C++17 编译器和 Python 3。`MVD_VECTOR_CACHE_BYTES` 控制向量缓存估算上限，默认 16 MiB；设为 `0` 可检查无缓存回退路径。缓存计费是估算值，仍须在正式容器中测量实际内存。

Dockerfile 固定 Python 3.12.16 slim 基础镜像的摘要，在构建阶段安装 C++ 编译器，从源码编译原生扫描库，并将编译产物复制进运行镜像。仓库根目录的 `.dockerignore` 将 seekdb 的其他大型源码排除在本镜像构建上下文之外。安装 Docker 后可执行：

```bash
docker build -t minivectordb:local .
docker run --rm -i -v minivectordb-data:/data minivectordb:local
make container-test
```

本地 WSL2 的 Docker daemon 禁用了桥接网络时，改用 `make container-test DOCKER_BUILD_FLAGS='--network host'`；常规 Docker 环境直接使用上面的命令。

当前调试协议中，一行一条完整 SQL，程序每行输出一个 JSON 对象；stdout 只写结果，日志使用 stderr。下面的示例使用空目录，覆盖写入、普通查询、向量检索、提交、回滚和重启：

```bash
data_dir=$(mktemp -d)
printf "%s\n" \
  "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(2));" \
  "INSERT INTO docs VALUES (1, 'a', '[0,0]');" \
  "SELECT id FROM docs ORDER BY id;" \
  "SELECT id, L2_DISTANCE(embedding, '[1,0]') AS distance FROM docs ORDER BY distance LIMIT 1;" \
  "BEGIN;" \
  "INSERT INTO docs VALUES (2, 'b', '[1,0]');" \
  "ROLLBACK;" \
  "BEGIN;" \
  "INSERT INTO docs VALUES (3, 'c', '[1,0]');" \
  "COMMIT;" | python3 minivectordb.py --data-dir "$data_dir"
printf "%s\n" "SELECT id FROM docs ORDER BY id;" | python3 minivectordb.py --data-dir "$data_dir"
```

错误返回 `error` 语义码，公开样例所需的 `duplicate_key`、`dimension_mismatch`、`unsupported_sql` 已实现。其他错误暂用 `sql_error`；最终错误码须以正式驱动为准。学习材料位于 `agent/conversation.json`，仅记录真实的学生提示和本地工具结果，明确区分来源。

`make scan` 扫描缓存、主键哈希桶和 WAL 压缩等级，结果与选择理由见 [PARAM_SCAN.md](PARAM_SCAN.md)。当前默认值为 16 MiB 缓存、32768 个桶、WAL 压缩 4 级；均可分别用 `MVD_VECTOR_CACHE_BYTES`、`MVD_BUCKETS` 和 `MVD_WAL_COMPRESSION_LEVEL` 环境变量覆盖。

公开 `mysqltest` 目录定义了 SQL 与驱动指令 `--restart`、`--mode ann`，但没有提供完整正式进程通信格式。`run_public_cases.py` 与 `run_process_cases.py` 在本地解释这些驱动指令；这不等于正式模式切换协议已经实现。

`run_perf_probe.py` 是固定种子的本地探针：10,000 行、128 维、1,000 次 TopK 查询，半数带过滤。压缩 1 级时 WAL 为 3.97 MiB；当前默认 4 级的最近一次运行测得写入 1.07 秒、查询 0.48 秒、WAL 3.48 MiB、峰值 RSS 32.9 MiB。每次查询仍执行精确距离计算。这些数字不是赛方评分，且未涵盖官方的磁盘 I/O 测量方式。

`make probe-cgroup` 仅适用于启用了 systemd 和 cgroup v2 的 Linux：进行三轮独立 scope 测量，启用 I/O 与内存统计，将数据放在 `/var/tmp` 的磁盘文件系统，并读取该 scope 的 `io.stat` 与 `memory.peak`，输出各项中位数。它仍是本地 API 探针；硬件、数据目录设备、端到端进程协议、预热和基线均与赛方环境不同，不能据此计算官方 E 或名次。

2026-11-09 在 WSL2 的三轮本地测量中，中位数为写入 1.372 秒、1000 次查询 0.499 秒、写入阶段物理 I/O 9.086 MiB、查询阶段物理 I/O 0 MiB、cgroup 峰值内存 31.406 MiB。查询读盘为 0 表示本地查询主要命中已装载的缓存；官方查询项目还会把装载、索引准备和预热的资源计入统计范围。

`run_large_data_smoke.py` 在 64 MiB 进程地址空间上限下写入、查询和重启恢复 17,000 行；记录文件约 66.9 MiB。这验证了一个超内存数据场景，尚不能替代正式资源测试。官方性能内存指标是容器 cgroup `memory.peak`，包含页缓存；本地 RSS 不能直接对比。

WAL 达到 8 MiB 后，数据库同步并原子发布 `checkpoint.dat`，随后原子替换已覆盖的 WAL。启动时从快照恢复表与主键桶，只重放 WAL 尾部。若进程在两次发布之间退出，快照仍能识别旧 WAL 中已覆盖的范围。旧版 WAL 可直接打开。`MVD_CHECKPOINT_BYTES` 可改阈值，`0` 停止生成新快照，已有快照仍可恢复。快照会复制当前堆文件，每次生成会增加一次全量磁盘写入，并保留旧记录占用的空间。

`make checkpoint-test` 验证旧 WAL 迁移、恢复途中连续两次 SIGKILL、快照发布和 WAL 替换边界。`TMPDIR=/var/tmp python3 run_checkpoint_recovery.py --benchmark` 对比基线 `76e637d`。在 WSL2 ext4 上写入同样的 30,000 条 128 维向量，交错启动五轮，初始化中位数由 1.731 秒降至 0.402 秒，WAL 由 10,975,931 字节降至 2,562,161 字节。向量缓存延迟加载，首条 TopK 的中位数由 0.159 秒升至 0.263 秒。

超大事务的暂存数据仍会占用内存。无 LIMIT 的大结果集会在返回前积累在内存中。当前版本还缺少近似索引和官方环境预检结果。
