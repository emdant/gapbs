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

KERNELS = bc bfs cc cc_sv pr pr_spmv tc
SUITE = $(KERNELS) converter sssp-int32 sssp-float sssp-crelax sssp-ctime

.PHONY: all sssp
all: $(SUITE)

% : src/%.cc src/*.h
	$(CXX) $(CXX_FLAGS) -DUSE_INT32 $< -o $@

sssp: sssp-int32 sssp-float

sssp-int32: src/sssp.cc src/*.h
	$(CXX) $(CXX_FLAGS) -DUSE_INT32 $< -o $@

sssp-float: src/sssp.cc src/*.h
	$(CXX) $(CXX_FLAGS) -DUSE_FLOAT $< -o $@
	
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
