CXX      := g++
CXXFLAGS := -std=c++17 -O3 -march=native -Wall -Wextra
LIBS     := -lrt

.PHONY: all clean

all: bench_producer bench_consumer

bench_producer: bench_producer.cpp ring_buffer.hpp
	$(CXX) $(CXXFLAGS) -o $@ bench_producer.cpp $(LIBS)

bench_consumer: bench_consumer.cpp ring_buffer.hpp
	$(CXX) $(CXXFLAGS) -o $@ bench_consumer.cpp $(LIBS)

clean:
	rm -f bench_producer bench_consumer
	# Also clean leftover SHM segments and result files
	rm -f /tmp/bench_shm*.* /tmp/latency_shm*.bin
