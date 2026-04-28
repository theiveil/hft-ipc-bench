CXX        := g++
CXXFLAGS   := -std=c++17 -O2 -march=native -Wall -Wextra

GRPC_CFLAGS := $(shell pkg-config --cflags grpc++ protobuf 2>/dev/null)
GRPC_LIBS   := $(shell pkg-config --libs   grpc++ protobuf 2>/dev/null) -lgrpc++

PROTOC       := protoc
GRPC_PLUGIN  := $(shell which grpc_cpp_plugin)

PROTO_SRCS   := bench.pb.cc bench.grpc.pb.cc
PROTO_HDRS   := bench.pb.h  bench.grpc.pb.h

# Install build dependencies (run once with sudo):
#   sudo apt-get install -y protobuf-compiler libgrpc++-dev \
#                           protobuf-compiler-grpc libprotobuf-dev
#
.PHONY: all clean proto install-deps

all: proto bench_server bench_client

# ── Code generation ────────────────────────────────────────────────────────────
proto: $(PROTO_SRCS)

bench.pb.cc bench.pb.h: bench.proto
	$(PROTOC) --cpp_out=. bench.proto

bench.grpc.pb.cc bench.grpc.pb.h: bench.proto
	$(PROTOC) --grpc_out=. --plugin=protoc-gen-grpc=$(GRPC_PLUGIN) bench.proto

# ── Binaries ───────────────────────────────────────────────────────────────────
bench_server: bench_server.cpp $(PROTO_SRCS) $(PROTO_HDRS)
	$(CXX) $(CXXFLAGS) $(GRPC_CFLAGS) -o $@ bench_server.cpp $(PROTO_SRCS) $(GRPC_LIBS)

bench_client: bench_client.cpp $(PROTO_SRCS) $(PROTO_HDRS)
	$(CXX) $(CXXFLAGS) $(GRPC_CFLAGS) -o $@ bench_client.cpp $(PROTO_SRCS) $(GRPC_LIBS)

install-deps:
	sudo apt-get install -y protobuf-compiler libgrpc++-dev \
	                        protobuf-compiler-grpc libprotobuf-dev

clean:
	rm -f bench_server bench_client
	rm -f $(PROTO_SRCS) $(PROTO_HDRS)
	rm -f /tmp/bench_grpc*.* /tmp/latency_grpc*.bin /tmp/bench_grpc.sock
