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
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused \
            -Woverloaded-virtual -Wdouble-promotion -Wformat=2
OPT      := -O3 -march=native -DNDEBUG
DEBUGOPT := -O0 -g3
INC      := -Iinclude

BUILD    := build
SRC      := $(wildcard src/*.cpp)
OBJ      := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRC))

TEST_SRC := $(wildcard tests/*.cpp)
BENCH_SRC:= $(wildcard bench/*.cpp)

.PHONY: all test bench asan replay clean fmt
all: $(BUILD)/pricetime_tests $(BUILD)/pricetime_bench $(BUILD)/pricetime_replay

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(STD) $(WARN) $(OPT) $(INC) -c $< -o $@

$(BUILD)/pricetime_tests: $(TEST_SRC) $(OBJ) | $(BUILD)
	$(CXX) $(STD) $(WARN) $(OPT) $(INC) $^ -o $@

$(BUILD)/pricetime_bench: $(BENCH_SRC) $(OBJ) | $(BUILD)
	$(CXX) $(STD) $(WARN) $(OPT) $(INC) $^ -o $@

$(BUILD)/pricetime_replay: src/replay_main.cpp $(filter-out $(BUILD)/replay_main.o,$(OBJ)) | $(BUILD)
	$(CXX) $(STD) $(WARN) $(OPT) $(INC) $^ -o $@

test: $(BUILD)/pricetime_tests
	@./$(BUILD)/pricetime_tests

bench: $(BUILD)/pricetime_bench
	@./$(BUILD)/pricetime_bench

replay: $(BUILD)/pricetime_replay
	@./$(BUILD)/pricetime_replay

# Sanitizers are a separate build dir so they never poison the timed binaries.
asan: | $(BUILD)
	@mkdir -p $(BUILD)/asan
	$(CXX) $(STD) $(WARN) $(DEBUGOPT) -fsanitize=address,undefined \
	  -fno-omit-frame-pointer $(INC) $(TEST_SRC) $(SRC) -o $(BUILD)/asan/tests
	@ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 ./$(BUILD)/asan/tests

clean:
	@rm -rf $(BUILD)
