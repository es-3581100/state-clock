CXX ?= g++
CPPFLAGS := -Iinclude
CXXFLAGS := -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -flto -pthread
LDFLAGS := -flto -pthread

ifeq ($(NATIVE),1)
CXXFLAGS += -march=native -mtune=native
endif

BUILD := build
BIN := $(BUILD)/nanoclock
OBJS := $(BUILD)/nanoclock.o $(BUILD)/main.o

.PHONY: all clean test bench native
all: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/nanoclock.o: src/nanoclock.cpp include/nanoclock.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/main.o: src/main.cpp include/nanoclock.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@

native:
	$(MAKE) clean
	$(MAKE) NATIVE=1 all

test: $(BIN)
	./$(BIN) --self-test

bench: $(BIN)
	./$(BIN) --bench

clean:
	rm -rf $(BUILD)
