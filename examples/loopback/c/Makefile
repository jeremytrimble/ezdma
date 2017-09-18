
CC=gcc
CFLAGS=-O2
LDFLAGS=

all: ezdma_send ezdma_receive ezdma_speed_test

ezdma_receive: stream_shared.o ezdma_receive.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

ezdma_send: stream_shared.o ezdma_send.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

ezdma_speed_test: ezdma_speed_test.c
	$(CC) $(CFLAGS) -o $@ $<

%.o: %c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f ezdma_speed_test ezdma_send ezdma_receive stream_shared.o ezdma_send.o ezdma_receive.o

.PHONY: clean
