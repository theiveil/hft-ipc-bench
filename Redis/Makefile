CXX      := g++
CXXFLAGS := -std=c++17 -O2 -march=native -Wall -Wextra
INCS     := $(shell pkg-config --cflags hiredis 2>/dev/null)
LIBS     := $(shell pkg-config --libs   hiredis 2>/dev/null || echo "-lhiredis")

.PHONY: all clean redis_server

all: producer consumer

producer: main_producer.cpp RedisProducer.cpp RedisProducer.hpp Message.hpp
	$(CXX) $(CXXFLAGS) $(INCS) -o $@ main_producer.cpp RedisProducer.cpp $(LIBS)

consumer: main_consumer.cpp RedisConsumer.cpp RedisConsumer.hpp Message.hpp
	$(CXX) $(CXXFLAGS) $(INCS) -o $@ main_consumer.cpp RedisConsumer.cpp $(LIBS)

# Start a dedicated Redis instance for benchmarking
redis_server:
	redis-server redis.conf --daemonize yes --logfile /tmp/redis_bench.log
	@echo "Redis started. Log: /tmp/redis_bench.log"

clean:
	rm -f producer consumer
	rm -f /tmp/bench_redis*.* /tmp/latency_redis*.bin
	redis-cli -s /tmp/redis_bench.sock SHUTDOWN NOSAVE 2>/dev/null || true
