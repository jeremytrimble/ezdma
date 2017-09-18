/*
ezdma loopback speed test
Copyright (C) 2015 Jeremy Trimble

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
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>

const int NUM_TRIALS = 100000;

#define PACKET_SIZE (4096)

uint8_t tx_buf[PACKET_SIZE];
uint8_t rx_buf[PACKET_SIZE];

int main(int argc, char *argv[])
{
    struct timespec tick, tock;
    int i;

    int tx_fd = open("/dev/loop_tx", O_WRONLY);
    int rx_fd = open("/dev/loop_rx", O_RDONLY);

    if ( tx_fd < 0 || rx_fd < 0 )
    {
        perror("can't open loop devices\n");
        return 2;
    }

    for (i = 0; i < PACKET_SIZE; ++i)
        tx_buf[i] = i; // automatically mod-256

    assert( !clock_gettime(CLOCK_MONOTONIC, &tick) );

    i = 0;
    while ( i < NUM_TRIALS )
    {
        //printf("trial %d\n", i);
        assert( PACKET_SIZE == write(tx_fd, tx_buf, PACKET_SIZE) );
        assert( PACKET_SIZE == read (rx_fd, rx_buf, PACKET_SIZE) );
        //{
        //int rv = read (rx_fd, rx_buf, PACKET_SIZE);
        //printf("read returned %d\n", rv);
        //}

        {
            int j;
            for (j = 0; j < PACKET_SIZE; ++j)
            {
                if ( rx_buf[j] != tx_buf[j] )
                {
                    printf("ERROR IN DATA\n");
                    printf("  @ j=%d: rx_buf[%d]: %u, tx_buf[%d]: %u\n",
                        j, j, rx_buf[j], j, tx_buf[j]);
                    return 2;
                }
            }

        }

        tx_buf[i % PACKET_SIZE] += 5;  // modify data each time

        i++;
    }

    assert( !clock_gettime(CLOCK_MONOTONIC, &tock) );

    {
        double start, end, diff, bytes_per_sec;
        double numBytes = (double)NUM_TRIALS * PACKET_SIZE;

        start = tick.tv_sec + tick.tv_nsec/1e9;
        end   = tock.tv_sec + tock.tv_nsec/1e9;
        diff  = end - start;

        bytes_per_sec = numBytes / (double)(1<<20) / diff;

        printf("sent %d %d-byte packets in %.9f sec: %.3f MB/s\n",
                NUM_TRIALS, PACKET_SIZE, diff, bytes_per_sec);
    }
    
    return 0;
}


