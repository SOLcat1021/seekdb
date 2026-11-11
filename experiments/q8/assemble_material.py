"""Append verified Q8 experiment excerpts without inventing student reflections."""
from pathlib import Path
import json

root = Path(__file__).resolve().parents[2]
path = root / 'agent/conversation.json'
data = json.loads(path.read_text(encoding='utf-8'))
assert not any(x['id'] == 'e13' for x in data['experiments']), 'already appended'
ev = root / 'experiments/q8/evidence'
read = lambda name: json.loads((ev/name).read_text(encoding='utf-8'))
compact = lambda value: json.dumps(value, ensure_ascii=False, separators=(',', ':'), allow_nan=False)
base = '28695d19460afa9ab2b501c31719b214427222d4'
candidate = '8d5fb44f51e8ad06d4f840c6db18f1e1ac04b034'
data['version'] = 'minivectordb-learning-record-5'
data['scope']['through_date'] = '2026-11-12'
data['scope']['source_revision'] = candidate
data['scope']['material_base_revision'] = '42d79faa65b164fa8c1413109d538e4e071a2872'
data['scope']['notes'] += ' 新增 8 位编码与保守下界筛选的真实请求及本地实验结果。实验、推导和实现由 Agent 完成；没有补造学生对新结果的解释。本次提测整合源码与材料，正式平台结果尚待回执。'
data['material_summary'] = '本材料包含学生真实提问、事后判断与预先设计的反例，并逐项区分 Agent 判断和工具执行结果。既有证据涵盖 checkpoint、分块 INSERT、响应字节上限、事务提交内存和跨表崩溃恢复、精确 TopK 大数反例。本次补充学生要求探索更大幅优化并指定 GCC13 同条件逐条核对，随后由 Agent 实现 8 位编码安全筛选并测试：1 万行基本持平；10 万行扣除首查后的不同查询耗时降低约 20.6%，但首查和内存上升。学生随后要求将该原型和材料一起提测。所有新增性能数字均为本地结果，未写成正式成绩，也未虚构学生的算法推导或结果复盘。'
start = len(data['conversations'])
data['conversations'].extend([
 {'id':'u11','source':'student','attribution':'verbatim','timing':'before_experiment_e13','content':'这些都是很微小的优化，你查阅一下资料，有没有什么办法有潜力能大幅提升，激进地省去一些工序，替换一些组件啥的'},
 {'id':'a10','source':'agent','attribution':'summary','timing':'before_experiment_e13','content':'Agent 提议在原有精确路径前增加 8 位编码与安全距离下界；只有能证明候选无法进入 TopK 才排除，其余保留原始向量精确复核。该算法推导及实现建议来自 Agent，不计作学生原话或独立发现。'},
 {'id':'u12','source':'student','attribution':'verbatim','timing':'before_experiment_e13','content':'好，先做你说的8位压缩加安全下界的精确查询原型，用GCC13和当前版本同条件对比，逐条核对结果，看看能不能有明显提升，先别动正式提测分支。'},
 {'id':'a11','source':'agent','attribution':'summary','timing':'after_experiment_e13','content':'Agent 实现二次幂缩放的有符号 8 位编码，按整数直接差平方和及保守阈值筛选，最终仍用原有精确距离与主键排序。首次单累加器实现略慢，改成四路累加后，1 万行基本持平；10 万行后续不同查询有收益，但建编码拖慢首查并增加内存。因此建议保留实验原型，暂不默认替换正式版。该判断来自 Agent，学生尚未提供个人复盘。'},
 {'id':'u13','source':'student','attribution':'verbatim','timing':'after_experiment_e13','content':'那就把这一版和我们的新的agent prompt整合起来提测一次看看'}
])
small = read('final-10k.json')
large = read('final-100k-diverse.json')
repeated = read('final-100k-repeated.json')
summary = read('derived-summary.json')
output = compact({'rows_10000':{'baseline':small['summary']['baseline'],'candidate':small['summary']['candidate']},'rows_100000':{'baseline':large['summary']['baseline'],'candidate':large['summary']['candidate']}})
unit = read('bound-check.json')
validation = compact({'public':read('public.json'),'exact':read('exact.json'),'large':read('large-correctness.json'),'recovery':read('recovery.json'),'cancellation':read('cancellation.json')})
idx = len(data['experiments'])
data['experiments'].append({
 'id':'e13','source':'tool_result','status':'executed_local','executed_by':'Agent 通过本地工具',
 'purpose':'8 位编码安全筛选的精确 TopK 原型，与当前工作版本 GCC13 同条件对照',
 'source_revision':{'baseline':base,'candidate':candidate},
 'environment':{'compiler':'GCC 13.5.0','flags':'-std=c++17 -O3 -flto; no fast-math','cpus':2,'memory':'2 GiB','image_ids':small['image_ids']},
 'method':'两引擎同工具链、同数据与查询，每版六轮，A/B/B/A、B/A/A/B、A/B/B/A 交替；每次新容器与数据目录。1 万行/128维/3000不同查询，种子20261110；10万行/128维/128不同查询，种子20261112。均K=10，一半等值过滤。独立标量真值逐条核对主键、排序和距离，真值计算不计入耗时。额外重复查询数据使用1024个原型及区分主键的末维，重复全零查询，不能替代不同随机查询。',
 'output':output,
 'derived_summary':compact(summary),
 'diagnostic_output':(ev/'screen-stats.stderr').read_text(encoding='utf-8').strip(),
 'validation_output':validation,
 'bound_check_output':unit['captured_tool_output'],
 'bound_check_configuration':unit['configuration'],
 'initial_trial_output':compact(read('initial-10k.json')['summary']),
 'repeated_query_output':compact(repeated['summary']),
 'limitations':'这些是本地测试，不是正式成绩。端到端时间包含控制器处理；没有统一预热，必须区分包含首查与扣除首查。原始浮点向量与编码并存，内存增加。诊断原子计数运行不用于性能结论。I/O 快照在 shutdown 前采集，不能当作完整官方 B 指标。ASan/UBSan 仅用于独立编码内核，不是本轮完整数据库 sanitizer。有限下界检查不是穷举证明。最初测试误断言所有查询都在编码区间，已修正为记录合法回退，未放宽查询结果核对。未取得本次正式评测回执。'
})
data['evidence_index'].append({
 'id':'q10',
 'student_evidence':[{'pointer':f'/conversations/{start}/content','quote':data['conversations'][start]['content']},{'pointer':f'/conversations/{start+2}/content','quote':data['conversations'][start+2]['content']}],
 'agent_context':[{'pointer':f'/conversations/{start+1}/content','quote':'只有能证明候选无法进入 TopK 才排除，其余保留原始向量精确复核。'}],
 'result_evidence':[{'pointer':f'/experiments/{idx}/derived_summary','quote':'"subsequent_reduction_percent":20.632216043520447'},{'pointer':f'/experiments/{idx}/derived_summary','quote':'"total_reduction_percent":-0.16362284570197172'},{'pointer':f'/experiments/{idx}/bound_check_output','quote':'"unsafe_prunes":0'},{'pointer':f'/experiments/{idx}/validation_output','quote':'"queries_verified":4800'}],
 'note':'学生实际贡献是要求改变优化方向，并约束工具链、同条件对照、逐条核对和先不动正式分支。算法推导、实现、测试与结果解释由 Agent 完成；没有学生本人解释新结果的证据。后续提测授权是操作决定，不据此推断学生已理解全部算法。'
})
path.write_bytes((json.dumps(data,ensure_ascii=False,indent=2,allow_nan=False)+'\n').encode('utf-8'))
print('conversation bytes:',path.stat().st_size)
