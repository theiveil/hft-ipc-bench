CXX      := g++
CXXFLAGS := -std=c++17 -O2 -march=native -Wall -Wextra
LIBS     := -lzmq

.PHONY: all clean

all: producer consumer

producer: main_producer.cpp Producer.cpp Producer.hpp Message.hpp
	$(CXX) $(CXXFLAGS) -o $@ main_producer.cpp Producer.cpp $(LIBS)

consumer: main_consumer.cpp Consumer.cpp Consumer.hpp Message.hpp
	$(CXX) $(CXXFLAGS) -o $@ main_consumer.cpp Consumer.cpp $(LIBS)

clean:
	rm -f producer consumer
	rm -f /tmp/bench_zmq*.* /tmp/latency_zmq*.bin
	rm -f /tmp/zmq_bench  # remove IPC socket file if it exists
