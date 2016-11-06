#!/bin/sh

./ezdma_receive &
RECV_PID=$!

./ezdma_send

wait $RECV_PID
