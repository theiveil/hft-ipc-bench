#### 1. Implement the Redis part according to the requirements in readme.md, using UDS.
- The code architecture provided by AI is good, but it doesn't consider that we need to record the time as soon as we receive the packet. It uses a string type for the message, which causes time to be spent parsing the packet, making the test unfair. Solution: Change to sending binary messages, so the consumer does not need to spend time parsing the format.

#### 2. What could be causing ZeroMQ to get stuck?
- The termination condition depends on seq_id == N_MESSAGES - 1, but ZeroMQ allows packet loss by default. If the last message is lost, the consumer will be stuck indefinitely.

#### 3. Generate a shm solution according to the requirements in readme.md.
- AI's approach always validating various values, such as verifying message correctness before reading it, which affects the fairness of latency testing. Therefore, I removed the validation process.

- Given that I needed lock-free operation, shared memory and multiple consumers, the AI's solution was very poor. It created an independent ring buffer for each consumer and used a for loop to copy the same message many times.

- Solution: The reason was that the RingBuffer struct built by AI only had one consumer cursor, which was shared by multiple consumers. Therefore, I modified the data structure to give each consumer its own independent cursor.

#### 4. Should all messages be distributed at once, or should the speed be controlled?
- The speed at which producers send messages should be limited because in an HFT scenario, we should simulate real-world, even market conditions. If we want to test the stress limit, we can continuously increase the MPS (mps).

#### 5. Should we use zero copy in zeroMQ product.cpp?
- zmq_msg_init_data() takes ownership of the buffer you provide and calls your supplied free function when it is no longer needed. However, this introduces additional problems:
Frequent allocation/deallocation
More complex lifecycle management
The overhead of calling the free function across threads
Therefore, zero-copy is not recommended for 64-byte messages.

#### 6. Change shm warmup to a two-level warmup:After the consumer attaches to shared memory, it first reads the seq field of all slots once to warm up the page table, cache, and TLB.The producer warmup should cover the entire ring.Add a barrier to prevent the producer warmup from lapping before the consumers are ready.

#### 7. Add a baseline implemented using Unix Domain Sockets (UDS):Support SPMC: one producer, multiple consumers.The producer should send the same message to all connected consumers, simulating SHM-style broadcast.Strictly preserve the 64-byte Message layout defined in shm/ring_buffer.hpp.Use the SOCK_STREAM approach.

#### 8. Update analyze_csv_results.ipynb to add analysis for the UDS benchmark data, and remove the Redis analysis section.

#### 9. Write a Python program to analyze the SHM CSV data. Process the 5 newshm*.csv files under the output folder.Remove extreme p50 outlier data.If the p50 value is abnormal, drop the entire row. For the remaining data, group by configuration and take the average. Output the result as shm_final.csv. analyze.py should no longer directly analyze the raw SHM data. Instead, it should merge shm_final.csv into analysis_summary.csv.