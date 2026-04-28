# hft-ipc-bench

>Benchmarking and Optimization of In-Host IPC Message Distribution Mechanisms in a High-Frequency Trading Scenario

## **1. Scenario Description**
This project simulates an **in-host** multi-process high-frequency market data distribution scenario. The core task is to test the performance limits of different inter-process communication mechanisms under extreme conditions: transmitting **fixed-size** micro messages **without persistence requirements**.

## **2. Comparison Baselines**
Three widely used and easy-to-use open-source solutions and one self-implemented highly optimized solution are selected:

- **Centralized In-Memory Store:** **Redis Pub/Sub** (Communicating via Unix Domain Sockets. Serves as a baseline for traditional IT architectures).

- **Standard RPC Framework:** **gRPC** (In-host calls. Represents a modern, widely adopted microservice communication standard).

- **Brokerless Message Queue:** **ZeroMQ (`ipc://` transport)** (A high-performance standard for asynchronous message passing).

- **Self-Implemented Solution (Lock-Free Shared Memory):** A highly optimized, lock-free ring buffer utilizing `shm_open` and `mmap`. This approach uses C++ `std::atomic` for lock-free Compare-and-Swap (CAS) operations and applies `alignas(64)` cache-line padding to head/tail pointers to eliminate false sharing, aiming for true zero-copy communication.

## **3. Evaluation Metrics**
To accurately reflect HFT requirements, performance will be evaluated across three dimensions:

- **Latency & Determinism (Crucial for HFT):**
    - End-to-end latency (measured from producer payload creation to consumer parsing).
    - Latency percentiles: **p50, p90, p99, p99.9, and Max Latency** (Tail latency is heavily penalized in HFT).

- **Throughput (Capacity):**
    - Messages processed per second (MPS) at peak load before queue buildup occurs.

- **Resource Footprint & Micro-architecture Profiling:**
    - CPU utilization and Context Switch rates (voluntary vs. involuntary).
    - Memory bandwidth consumption.
#### **Detailed, practical definitions**
1. Latency & Determinism:

	1.1  End-to-End Latency: 
	 - **Start Point (T1):** After the Producer constructs the message, but _right before_ the actual send call (for gRPC/ZeroMQ) or after the payload is fully written, at the publication boundary where the message becomes visible to consumers (for shared memory). 
    - **End Point (T2):** While polling or blocking, the exact moment the Consumer successfully gets the message pointer or finishes deserialization (depending on the IPC tool).
    - **How to Calculate:** The Consumer extracts `send_tsc` (T1) from the message and calculates `T2 - T1`. This difference is in CPU cycles. Later, during **offline analysis (after the test)**, this is converted into nanoseconds (ns) using the CPU frequency.
    
	1.2 Latency Percentiles: p50, p90, p99, p99.9
	- **Data Collection:** The Consumer records every single one-way latency during the entire test window (e.g., across 10 million messages).
	- **How to Calculate (Offline Analysis):** After the benchmark finishes, sort all 10 million latency data points in ascending order.

2. Throughput

	2.1 Max MPS - Messages Per Second
	- **Start Point (S1):** The Producer prepares to send a massive, fixed batch (e.g., 50 million messages). It records the system time `S1` right before sending the very first message.
	- **End Point (S2):** The Consumer tracks the Sequence Numbers. When it receives the very last message of that batch, it records the system time `S2`.
	- **Validation:** You must check the Sequence Numbers to ensure there is no packet loss (for Redis/ZeroMQ) or queue overflow/congestion (for shared memory). The calculated throughput is only valid if there is zero message loss and the Consumer isn't experiencing severe lag.
	- **How to Calculate:** `MPS = Total Messages / (S2 - S1)`

3. Resource Footprint & Micro-architecture
	
    3.1 CPU Load & Context Switches
	- **Tools:** `pidstat -w -p <PID> 1` or `perf`.
	- **Metrics to Track:**
	    - **%usr / %system:** User-space vs. Kernel-space CPU usage. A highly optimized shared memory setup should sit at nearly 100% `%usr`. Meanwhile, gRPC or Redis will likely show higher `%system` usage because they rely on system calls.
	    - **cswch/s (Voluntary Context Switches):** Happens when a process actively blocks or sleeps to wait for a message.
	    - **nvcswch/s (Involuntary Context Switches):** Happens when a process runs out of its time slice and is kicked off the CPU by the OS. In HFT, the goal is to pin threads to specific cores and use 100% polling, pushing both of these context switch metrics as close to 0 as possible.

## **4. Testing Methodology**

1. **Scale Variation:** Testing with 1 Producer to 1, 2 and 4 Consumers.
2. **Message Rate Ramp-up:** Pushing messages at controlled rates (e.g., 2M msg/sec, 5M msg/sec, 10M msg/sec).
3. **Environment Binding:** Utilizing `numactl` and `taskset` to bind producer and consumer processes to specific CPU cores, testing performance when processes share the same L3 cache vs. crossing NUMA nodes.
