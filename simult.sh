#!/bin/bash

# IP Address, Port, Resolution, and Protocol
IP="172.17.234.148"
PORT="8080"
RESOLUTION="480p"
PROTOCOL="UDP"

# Number of terminals to open
NUM_TERMINALS=12

# Loop to open terminals
for i in $(seq 1 $NUM_TERMINALS); do
    gnome-terminal -- bash -c "./client $IP $PORT $RESOLUTION $PROTOCOL; exec bash"
done
