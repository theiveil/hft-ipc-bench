CXX      := g++
CXXFLAGS := -std=c++17 -O3 -march=native -Wall -Wextra

.PHONY: all clean

all: producer consumer

producer: main_producer.cpp common.hpp ../shm/ring_buffer.hpp
	$(CXX) $(CXXFLAGS) -o $@ main_producer.cpp

consumer: main_consumer.cpp common.hpp ../shm/ring_buffer.hpp
	$(CXX) $(CXXFLAGS) -o $@ main_consumer.cpp

clean:
	rm -f producer consumer
	rm -f /tmp/bench_uds*
