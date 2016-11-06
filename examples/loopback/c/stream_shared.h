/* Shared values for stream speed test */

#ifndef STREAM_SHARED_H
#define STREAM_SHARED_H

#include <time.h>
#include <stdint.h>

#define PACKET_SIZE (4096)

extern const int NUM_TRIALS;
void prepare_tx_buffer(uint8_t (*tx_buf)[PACKET_SIZE]);
void change_tx_buffer(uint8_t (*tx_buf)[PACKET_SIZE], int i);
int check_buffer(uint8_t (*rx_buf)[PACKET_SIZE],  uint8_t (*tx_buf)[PACKET_SIZE]);
void print_throughput(struct timespec *tick, struct timespec *tock);

#endif // STREAM_SHARED_H

