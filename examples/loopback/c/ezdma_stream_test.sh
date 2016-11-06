#!/bin/sh

./ezdma_receive &
RECV_PID=$!

sleep 0.1

./ezdma_send

wait $RECV_PID
