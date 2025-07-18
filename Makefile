# See LICENSE.txt for license details.

CXX_FLAGS += -std=c++11 -O3 -Wall -march=native
PAR_FLAG = -fopenmp

ifneq (,$(findstring icpc,$(CXX)))
	PAR_FLAG = -openmp
endif

ifneq (,$(findstring sunCC,$(CXX)))
	CXX_FLAGS = -std=c++11 -xO3 -m64 -xtarget=native
	PAR_FLAG = -xopenmp
endif

ifneq ($(SERIAL), 1)
	CXX_FLAGS += $(PAR_FLAG)
endif

KERNELS = bc bfs cc cc_sv pr pr_spmv sssp tc
SUITE = $(KERNELS) converter

.PHONY: all
all: $(SUITE)

% : src/%.cc src/*.h
	$(CXX) $(CXX_FLAGS) $< -o $@

sssp-crelax: src/sssp.cc src/*.h
	$(CXX) $(CXX_FLAGS) -DCOUNT_RELAX $< -o $@

sssp-ctime: src/sssp.cc src/*.h
	$(CXX) $(CXX_FLAGS) -DCOUNT_TIME $< -o $@

# Testing
include test/test.mk

# Benchmark Automation
include benchmark/bench.mk


.PHONY: clean
clean:
	rm -f $(SUITE) test/out/*
