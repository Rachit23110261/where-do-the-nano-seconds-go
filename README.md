# where-do-the-nano-seconds-go
Per-stage latency of a NASDAQ ITCH 5.0 market-data pipeline, measured with an rdtsc flight recorder: UDP replay → lock-free SPSC queue → order book. p50 581 ns from recv to book update; the p99.9 tail (1.3 ms) traced to a periodic ~2 ms stall. Book verified over a full trading day (268M messages, empty at close).
