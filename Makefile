# pricetime - zero-dependency build. Requires only g++ with C++20 and make.
#
#   make            build everything into build/
#   make test       build + run the correctness and differential suites
#   make bench      build + run the latency/throughput harness
#   make asan       rebuild with AddressSanitizer + UBSan and run tests
#   make replay     build + run the terminal book replay
#   make clean

CXX      ?= g++
STD      := -std=c++20
LDLIBS   ?= -pthread
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused \
            -Woverloaded-virtual -Wdouble-promotion -Wformat=2
OPT      := -O3 -march=native -DNDEBUG
DEBUGOPT := -O0 -g3
INC      := -Iinclude

BUILD    := build
# Library sources: everything in src/ that is not an executable entry point.
LIB_SRC  := $(filter-out %_main.cpp,$(wildcard src/*.cpp))
LIB_OBJ  := $(patsubst src/%.cpp,$(BUILD)/%.o,$(LIB_SRC))

TEST_SRC := $(wildcard tests/*.cpp)
BENCH_SRC:= bench/bench_main.cpp

.PHONY: all test bench shardbench asan tsan replay iex recover nbbo clean fmt
all: $(BUILD)/pricetime_tests $(BUILD)/pricetime_bench $(BUILD)/pricetime_replay $(BUILD)/pricetime_iex $(BUILD)/pricetime_shardbench $(BUILD)/pricetime_recover $(BUILD)/pricetime_nbbo

$(BUILD):
	@mkdir -p $(BUILD)

# -MMD -MP emits a .d file per object listing the headers it included, so
# editing a header rebuilds everything that depends on it. Without this a
# changed header leaves stale objects that either link against a vanished
# symbol or, far worse, link successfully against a struct whose layout moved.
$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(STD) $(WARN) $(OPT) $(INC) -MMD -MP -c $< -o $@

-include $(LIB_OBJ:.o=.d)

$(BUILD)/pricetime_tests: $(TEST_SRC) $(LIB_OBJ) | $(BUILD)
	$(CXX) $(STD) $(WARN) $(OPT) $(INC) $^ -o $@ $(LDLIBS)

$(BUILD)/pricetime_bench: $(BENCH_SRC) $(LIB_OBJ) | $(BUILD)
	$(CXX) $(STD) $(WARN) $(OPT) $(INC) $^ -o $@ $(LDLIBS)

$(BUILD)/pricetime_replay: src/replay_main.cpp $(LIB_OBJ) | $(BUILD)
	$(CXX) $(STD) $(WARN) $(OPT) $(INC) $^ -o $@ $(LDLIBS)

$(BUILD)/pricetime_iex: src/replay_iex_main.cpp $(LIB_OBJ) | $(BUILD)
	$(CXX) $(STD) $(WARN) $(OPT) $(INC) $^ -o $@ $(LDLIBS)

$(BUILD)/pricetime_shardbench: bench/bench_shard.cpp $(LIB_OBJ) | $(BUILD)
	$(CXX) $(STD) $(WARN) $(OPT) $(INC) $^ -o $@ $(LDLIBS)

$(BUILD)/pricetime_recover: src/recover_main.cpp $(LIB_OBJ) | $(BUILD)
	$(CXX) $(STD) $(WARN) $(OPT) $(INC) $^ -o $@ $(LDLIBS)

$(BUILD)/pricetime_nbbo: src/nbbo_main.cpp $(LIB_OBJ) | $(BUILD)
	$(CXX) $(STD) $(WARN) $(OPT) $(INC) $^ -o $@ $(LDLIBS)

test: $(BUILD)/pricetime_tests
	@./$(BUILD)/pricetime_tests

bench: $(BUILD)/pricetime_bench
	@./$(BUILD)/pricetime_bench

shardbench: $(BUILD)/pricetime_shardbench
	@./$(BUILD)/pricetime_shardbench

replay: $(BUILD)/pricetime_replay
	@./$(BUILD)/pricetime_replay

recover: $(BUILD)/pricetime_recover
	@./$(BUILD)/pricetime_recover

nbbo: $(BUILD)/pricetime_nbbo
	@echo 'usage: ./scripts/feed_crypto.py | ./build/pricetime_nbbo'

iex: $(BUILD)/pricetime_iex
	@echo 'usage: ./build/pricetime_iex <IEX_DPLS.pcap.gz> [SYMBOL]'

# Sanitizers are a separate build dir so they never poison the timed binaries.
asan: | $(BUILD)
	@mkdir -p $(BUILD)/asan
	$(CXX) $(STD) $(WARN) $(DEBUGOPT) -fsanitize=address,undefined \
	  -fno-omit-frame-pointer $(INC) $(TEST_SRC) $(LIB_SRC) -o $(BUILD)/asan/tests $(LDLIBS)
	@ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./$(BUILD)/asan/tests

# ThreadSanitizer. Separate build dir, and ASLR is disabled via setarch
# because TSan aborts with "unexpected memory mapping" on WSL2 kernels
# otherwise. This target found a real data race in the seqlock: the payload
# was a plain struct, which works on x86 and is undefined behaviour anyway.
tsan: | $(BUILD)
	@mkdir -p $(BUILD)/tsan
	$(CXX) $(STD) -O1 -g -fsanitize=thread -fno-omit-frame-pointer $(INC) \
	  tests/main.cpp tests/test_sharded.cpp $(LIB_SRC) -o $(BUILD)/tsan/tests $(LDLIBS)
	@TSAN_OPTIONS=halt_on_error=0 setarch $$(uname -m) -R ./$(BUILD)/tsan/tests

clean:
	@rm -rf $(BUILD)
