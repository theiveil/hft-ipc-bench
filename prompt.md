##### 1. Implement the Redis part according to the requirements in readme.md, using UDS.
- The code architecture provided by AI is good, but it doesn't consider that we need to record the time as soon as we receive the packet. It uses a string type for the message, which causes time to be spent parsing the packet, making the test unfair. Solution: Change to sending binary messages, so the consumer does not need to spend time parsing the format.

##### 2. Write a script that can test four scenarios at once, with N_message values ​​of 100'000, 1'000'000, and 10'000'000, and with 1, 2, and 4 consumers.
- Later modifications were made, adding cases where mps is 2'000'000, 5'000'000, and 10'000'000.

##### 3. What could be causing ZeroMQ to get stuck in a multi-consumer environment?
- The termination condition depends on seq_id == N_MESSAGES - 1, but ZeroMQ allows packet loss by default. If the last message is lost, the consumer will be stuck indefinitely.

##### 4. Generate a shm solution according to the requirements in readme.md.
- AI's approach always validating various values, such as verifying message correctness before reading it, which affects the fairness of latency testing. Therefore, I removed the validation process.

- Given that I needed lock-free operation, shared memory and multiple consumers, the AI's solution was very poor. It created an independent ring buffer for each consumer and used a for loop to copy the same message many times.

- Solution: The reason was that the RingBuffer struct built by AI only had one consumer cursor, which was shared by multiple consumers. Therefore, I modified the data structure to give each consumer its own independent cursor.

##### 5. Should all messages be distributed at once, or should the speed be controlled?
- The speed at which producers send messages should be limited because in an HFT scenario, we should simulate real-world, even market conditions. If we want to test the stress limit, we can continuously increase the MPS (mps).

##### 6. Should we use zero copy in zeroMQ product.cpp?
- zmq_msg_init_data() takes ownership of the buffer you provide and calls your supplied free function when it is no longer needed. However, this introduces additional problems:
Frequent allocation/deallocation
More complex lifecycle management
The overhead of calling the free function across threads
Therefore, zero-copy is not recommended for 64-byte messages.

