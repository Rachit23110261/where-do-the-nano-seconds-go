# =============================================================================
# low-latency/Makefile - build + automated experiments for the ITCH latency lab
#
#   make                 build everything into build/
#   make help            list targets and tunable variables
#   make lat             fixed-rate latency run (default 100k msg/s)
#   make max             max-rate throughput run
#   make sweep           latency runs at several rates
#   make loss            default vs large SO_RCVBUF (single-thread receiver)
#   make book            offline order-book correctness check
#   make unit            SPSC stress test + ThreadSanitizer (if spsc_test.cpp exists)
#   make test            unit + book + lat   (the "did I break anything?" target)
#
# Override anything on the command line, e.g.
#   make lat RATE=200000 COUNT=2000000 PARSE_CPU=6
#   make sweep RATES="50000 100000 300000 600000"
# Results land in results/<timestamp>_<name>/ and one line per run in
# results/summary.csv.
# =============================================================================

# ---- toolchain ---------------------------------------------------------------
CXX       ?= g++
OPT       ?= -O2
CXXFLAGS  ?= $(OPT) -std=c++20 -Wall -Wextra
LDFLAGS   ?=
PY        ?= python3

# ---- experiment parameters ----------------------------------------------------
DATA       ?= itch/test1.itch
COUNT      ?= 5000000
RATE       ?= 100000
RATES      ?= 50000 100000 200000 400000
PORT       ?= 9999
RCVBUF     ?= 33554432
SAMPLE     ?= 1
REPLAY_CPU ?= 2
RX_CPU     ?= 3
PARSE_CPU  ?= 4
LOG_CPU    ?= 5
TAG        ?=

# ---- layout ---------------------------------------------------------------------
B        := build
INC      := -Ispsc -Iorderbook -Ireciever
RUN      := scripts/run_pipeline.sh
SPSC_TEST_SRC := $(wildcard spsc/spsc_test.cpp)

BINS := $(B)/itch_replay $(B)/itch_count $(B)/itch_book \
        $(B)/itch_recv $(B)/itch_rev2 $(B)/itch_rev3
ifneq ($(SPSC_TEST_SRC),)
BINS += $(B)/spsc_test $(B)/spsc_test_tsan
endif

# Every experiment passes its settings to the runner through the environment.
export DATA COUNT PORT RCVBUF SAMPLE REPLAY_CPU RX_CPU PARSE_CPU LOG_CPU PY TAG
export BUILD_FLAGS := $(CXXFLAGS)

.PHONY: all help clean distclean unit book lat max sweep loss test

all: $(BINS)

# ---- build rules ----------------------------------------------------------------
$(B):
	@mkdir -p $@

$(B)/itch_replay: itch/itch_replay.cpp | $(B)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

$(B)/itch_count: itch/itch_count.cpp | $(B)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

$(B)/itch_book: orderbook/itch_book.cpp orderbook/orderbook.hpp | $(B)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< $(LDFLAGS)

$(B)/itch_recv: reciever/itch_rev.cpp | $(B)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

$(B)/itch_rev2: reciever/itch_rev2.cpp spsc/spsc.hpp orderbook/orderbook.hpp | $(B)
	$(CXX) $(CXXFLAGS) -pthread $(INC) -o $@ $< $(LDFLAGS)

$(B)/itch_rev3: reciever/itch_rev3.cpp reciever/tsc.hpp spsc/spsc.hpp orderbook/orderbook.hpp | $(B)
	$(CXX) $(CXXFLAGS) -pthread $(INC) -o $@ $< $(LDFLAGS)

$(B)/spsc_test: spsc/spsc_test.cpp spsc/spsc.hpp | $(B)
	$(CXX) $(CXXFLAGS) -pthread $(INC) -o $@ $<

$(B)/spsc_test_tsan: spsc/spsc_test.cpp spsc/spsc.hpp | $(B)
	$(CXX) -O1 -g -std=c++20 -pthread -fsanitize=thread $(INC) -o $@ $<

# ---- experiments ------------------------------------------------------------------
unit: $(BINS)
ifeq ($(SPSC_TEST_SRC),)
	@echo "unit: spsc/spsc_test.cpp not found - skipping"
else
	./$(B)/spsc_test 100000000
	./$(B)/spsc_test_tsan 2000000
endif

book: $(B)/itch_book
	@mkdir -p results
	./$(B)/itch_book $(DATA) AAPL --count $(COUNT) | tee results/book_check.txt
	@grep -q "missing_ref  = 0" results/book_check.txt && \
	 grep -q "over_exec    = 0" results/book_check.txt && \
	 grep -q "dup_ref      = 0" results/book_check.txt && \
	 echo "BOOK CHECK: PASS" || { echo "BOOK CHECK: FAIL"; exit 1; }

lat: $(B)/itch_rev3 $(B)/itch_replay
	MODE=rev3 RATE=$(RATE) NAME=lat$(RATE) $(RUN)

max: $(B)/itch_rev3 $(B)/itch_replay
	MODE=rev3 RATE=0 NAME=max $(RUN)

sweep: $(B)/itch_rev3 $(B)/itch_replay
	@for r in $(RATES); do \
	   MODE=rev3 RATE=$$r NAME=sweep$$r $(RUN) || exit 1; \
	 done
	@echo; column -s, -t < results/summary.csv | tail -n +1

# Loss experiment: single-thread receiver, default kernel buffer vs RCVBUF.
loss: $(B)/itch_recv $(B)/itch_replay
	MODE=recv RATE=$(RATE) RCVBUF=0         NAME=loss_default $(RUN)
	MODE=recv RATE=$(RATE) RCVBUF=$(RCVBUF) NAME=loss_big     $(RUN)

test: unit book lat

# ---- housekeeping ------------------------------------------------------------------
clean:
	rm -rf $(B)

distclean: clean
	rm -rf results

help:
	@sed -n '2,20p' Makefile | sed 's/^# \{0,1\}//'
	@echo "Variables (current values):"
	@echo "  DATA=$(DATA) COUNT=$(COUNT) RATE=$(RATE) RATES='$(RATES)'"
	@echo "  PORT=$(PORT) RCVBUF=$(RCVBUF) SAMPLE=$(SAMPLE) TAG=$(TAG)"
	@echo "  CPUs: REPLAY=$(REPLAY_CPU) RX=$(RX_CPU) PARSE=$(PARSE_CPU) LOG=$(LOG_CPU)"
	@echo "  CXXFLAGS=$(CXXFLAGS)"

