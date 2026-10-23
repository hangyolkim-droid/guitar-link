CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
BIN      := amp_sim

.PHONY: all test demo clean

all: $(BIN)

$(BIN): src/main.cpp src/amp.h src/dsp.h
	$(CXX) $(CXXFLAGS) -o $@ src/main.cpp

test: $(BIN)
	./$(BIN) --selftest
	python3 latency_budget.py > /dev/null && echo "latency_budget: ok"
	python3 latency_mode.py --selftest
	python3 measure_latency.py --selftest
	python3 calibrate.py --selftest

demo: $(BIN)
	mkdir -p demo
	./$(BIN) --demo demo

clean:
	rm -f $(BIN)
	rm -rf __pycache__ demo
