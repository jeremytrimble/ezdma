
CC=gcc
CFLAGS=-O2

all: ezdma_send ezdma_receive ezdma_speed_test

ezdma_receive: ezdma_receive.c
	$(CC) $(CFLAGS) -o $@ $<

ezdma_send: ezdma_send.c
	$(CC) $(CFLAGS) -o $@ $<

ezdma_speed_test: ezdma_speed_test.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f ezdma_speed_test ezdma_send ezdma_receive

.PHONY: clean
