"""Audit every applicable recall, ID, memory and functional gate after measurement."""
import argparse
import hashlib
import json
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', required=True)
    parser.add_argument('--label', default='recall-v9')
    parser.add_argument('--probe-version', default='v9')
    parser.add_argument('--bridge')
    parser.add_argument('--out')
    args = parser.parse_args()
    work = Path(args.work)
    bridge = Path(args.bridge) if args.bridge else work / f'bridge-{args.probe_version}/libseekdb_cuvs_bridge.so'
    bridge_hash = hashlib.sha256(bridge.read_bytes()).hexdigest()
    result = {'scope': 'public-data acceptance, not an official hidden-data score',
              'label': args.label, 'bridge_sha256': bridge_hash,
              'sql': {}, 'distribution_checks': {}, 'failures': []}
    sql = work / f'sql-{args.label}'
    for dataset in ('sift', 'nytimes', 'synthetic256'):
        summary = json.loads((sql / dataset / 'summary.json').read_text())
        gates = summary['gates']
        excluded = {'candidate_recall10_abs', 'candidate_recall100_abs'} if dataset == 'nytimes' else set()
        required = {name: value for name, value in gates.items() if name not in excluded}
        # A missing measurement is not a pass.
        failed = [name for name, value in required.items() if value is not True]
        result['sql'][dataset] = {
            'required_gates': required, 'excluded_raw_proxy_absolute_gates': sorted(excluded),
            'all_applicable_gates_pass': not failed,
            **{name: value for name, value in summary.items()
               if name in ('build_ratio', 'query_ratio') or 'recall' in name or 'vram' in name}}
        result['failures'].extend(f'sql:{dataset}:{name}' for name in failed)
    provenance = json.loads((sql / 'provenance.json').read_text())
    if provenance['candidate_bridge_sha256'] != bridge_hash:
        result['failures'].append('sql_bridge_hash_mismatch')
    for dataset in ('gaussian', 'unit_sphere', 'anisotropic', 'clustered', 'low_rank', 'nytimes'):
        path = work / 'recall-screen' / dataset
        baseline = json.loads((path / 'baseline/result.json').read_text())
        candidate = json.loads((path / args.probe_version / 'result.json').read_text())
        item = {}
        for k in (10, 100):
            delta = candidate[f'recall{k}'] - baseline[f'recall{k}']
            item[f'recall{k}_delta'] = delta
            item[f'recall{k}_pass'] = delta >= -.005
            item[f'valid_ids{k}'] = candidate[f'invalid{k}'] == 0
        item['source_hash_matches'] = candidate['bridge_sha256'] == bridge_hash
        item['vram_ratio'] = candidate['vram_delta'] / baseline['vram_delta']
        item['vram_pass'] = item['vram_ratio'] <= 1.25
        item['all_applicable_gates_pass'] = all(value for value in item.values()
            if isinstance(value, bool))
        result['distribution_checks'][dataset] = item
        if not item['all_applicable_gates_pass']:
            result['failures'].append(f'distribution:{dataset}')
    functional = json.loads((work / 'bounded-functional-all.json').read_text())
    result['functional_all_pass'] = functional.get('all_checks_pass') is True
    result['functional_source_hash_matches'] = functional.get('candidate_bridge_sha256') == bridge_hash
    if not result['functional_all_pass']:
        result['failures'].append('functional')
    if not result['functional_source_hash_matches']:
        result['failures'].append('functional_bridge_hash_mismatch')
    result['weighted_public_performance'] = {}
    for proxy in ('nytimes', 'synthetic256'):
        build = result['sql']['sift']['build_ratio'] ** .4 * result['sql'][proxy]['build_ratio'] ** .6
        query = result['sql']['sift']['query_ratio'] ** .4 * result['sql'][proxy]['query_ratio'] ** .6
        result['weighted_public_performance'][proxy] = {
            'build': build, 'query': query,
            'meets_performance_thresholds': build >= 3 and query >= 1.5}
    result['all_applicable_gates_pass'] = not result['failures']
    output = Path(args.out) if args.out else work / f'{args.label}-final-audit.json'
    output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)
    return 0 if result['all_applicable_gates_pass'] else 1


if __name__ == '__main__':
    sys.exit(main())
