/*
ezdma speedtest shared functions
Copyright (C) 2015 Jeremy Trimble
Copyright (C) 2016 Jan Binder

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include "stream_shared.h"

const int NUM_TRIALS = 100000;

void prepare_tx_buffer(uint8_t (*tx_buf)[PACKET_SIZE])
{
    int i;
    for (i = 0; i < PACKET_SIZE; ++i)
        (*tx_buf)[i] = i; // automatically mod-256
}

void change_tx_buffer(uint8_t (*tx_buf)[PACKET_SIZE], int i)
{
    (*tx_buf)[i % PACKET_SIZE] += 5;  // modify data each time
}

int check_buffer(uint8_t (*rx_buf)[PACKET_SIZE], uint8_t (*tx_buf)[PACKET_SIZE])
{
    int i;
    for (i = 0; i < PACKET_SIZE; ++i)
    {
        if ( (*rx_buf)[i] != (*tx_buf)[i] )
        {
            printf("ERROR IN DATA\n");
            printf("  @ i=%d: rx_buf[%d]: %u, tx_buf[%d]: %u\n",
                i, i, (*rx_buf)[i], i, (*tx_buf)[i]);
            return 2;
        }
    }
    return 0;
}

void print_throughput(struct timespec *tick, struct timespec *tock)
{
    double start, end, diff, bytes_per_sec;
    double numBytes = (double)NUM_TRIALS * PACKET_SIZE;
    start = tick->tv_sec + tick->tv_nsec/1e9;
    end   = tock->tv_sec + tock->tv_nsec/1e9;
    diff  = end - start;

    bytes_per_sec = numBytes / (double)(1<<20) / diff;

    printf("sent %d %d-byte packets in %.9f sec: %.3f MB/s\n",
            NUM_TRIALS, PACKET_SIZE, diff, bytes_per_sec);
}

