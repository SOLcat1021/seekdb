CXX ?= c++
CXXFLAGS ?= -O3 -std=c++17 -fPIC -Wall -Wextra
DOCKER_BUILD_FLAGS ?=

libmvdscan.so: native_scan.cpp
	$(CXX) $(CXXFLAGS) -shared $< -o $@

.PHONY: test clean
test: libmvdscan.so
	python3 run_public_cases.py
	python3 run_process_cases.py
	python3 run_recovery_smoke.py
	python3 run_native_correctness.py
	python3 run_large_data_smoke.py

.PHONY: probe
probe: libmvdscan.so
	python3 run_perf_probe.py

.PHONY: probe-cgroup
probe-cgroup: libmvdscan.so
	python3 run_cgroup_benchmark.py

.PHONY: container-test
container-test:
	docker build $(DOCKER_BUILD_FLAGS) -t minivectordb:local .
	python3 run_process_cases.py --docker-image minivectordb:local

.PHONY: scan
scan: libmvdscan.so
	python3 scan_params.py

clean:
	rm -f libmvdscan.so
