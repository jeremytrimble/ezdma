/*
ezdma loopback stream sender
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
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>

#include "stream_shared.h"

uint8_t tx_buf[PACKET_SIZE];

int main(int argc, char *argv[])
{
    struct timespec tick, tock;
    int i;

    int tx_fd = open("/dev/loop_tx", O_WRONLY);

    if ( tx_fd < 0 )
    {
        perror("can't open sender loop device\n");
        return 2;
    }

    prepare_tx_buffer(&tx_buf);

    assert( !clock_gettime(CLOCK_MONOTONIC, &tick) );
    printf("Sending %d %d-byte packets\n", NUM_TRIALS, PACKET_SIZE);
    for (i = 0; i < NUM_TRIALS; ++i)
    {
        //printf("trial %d\n", i);
        assert( PACKET_SIZE == write(tx_fd, tx_buf, PACKET_SIZE) );
        change_tx_buffer(&tx_buf, i);
    }

    assert( !clock_gettime(CLOCK_MONOTONIC, &tock) );

    print_throughput(&tick, &tock);

    return 0;
}

