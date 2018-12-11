# Memcached with Shared Memory 

For the original implementation of memcached, please refer the the github link below:
https://github.com/memcached/memcached

## Dependencies
* libevent, http://www.monkey.org/~provos/libevent/ (libevent-dev)
* librt, in order to allow the use of POSIX shared memory
* automake, autotools-dev, and autoconf

## Changes and Motivation
Implemented a new mechanism that allocates from a shared pool of memory reserved only for memcached instances. This allows for a global view of memcached information and allows for future optimizations and more efficient slab allocation algorithms.
Currently, if instances are run using -G (greedy) they will request to take memory as much as they can, ignoring the memory limit set for them when starting instances.

## How to run Memcached
* ./autogen.sh 
* ./configure (Edit the LIBS in generated makefile to include -lrt and add -w to CFLAGS)
* make (make sure you have librt, libevent installed on your system)
* Compile init_share and stop_share (i.e. gcc -o init_share init_share.c -lrt -pthread)
* Run ./init_share XXXX (MB) with the total amount of shared memory you need.
* Run memcached instances (i.e. ./memcached -p 11212 -t 4 -m 4096 -n 550 -G).<br />
  Option G indicates the greedy approach of memory allocation, if not specified, instances will get memory equal to the limit set with -m.
* Once finished run ./stop_share to unlink the shared memory segment

## Benchmark
For benchmarking, the data caching package provided in Cloudsuite can be used. You have to have docker installed on your system. 
* Pull the client image from the docker hub (i.e. docker pull aamh/data-caching:client)
* docker run -it --name dc-client --network="host" aamh/data-caching:client bash
* cd usr/src/memcached/Cloudsuite-Client/memcached_client/
* vim docker_servers.txt 
* Edit this file to look like this:<br />
  127.0.0.1, 11212, 1<br />
  127.0.0.1, 11213, 2<br />
  ....<br />
  This file should contain one line for each memcached instances you want to test. First column is the IP address(127.0.0.1 is the localhost in the client container). Second column specifies the port the server is listening to. And the last column specifies the reverse ratio of number of request sent to the instance (1 means full speed, 2 means half,..).
* ./loader -a ../twitter_dataset/twitter_dataset_unscaled -o ../twitter_dataset/twitter_dataset_30x -s docker_servers.txt -w 4 -S 30 -D 6000 -j -T 1 -r 400000
* If data set already exists, to warm up the servers: ./loader -a ../twitter_dataset/twitter_dataset_30x -s docker_servers.txt -w 4 -S 1 -D 6000 -j -T 1 -r 400000
* Run the benchmark: ./loader -a ../twitter_dataset/twitter_dataset_30x -s docker_servers.txt -g 0.8 -T 1 -c 200 -w 8 -r 400000

For the full description of arguments and a more comprehensive guide, please refer to the Cloudsuite hompage:
http://cloudsuite.ch///pages/benchmarks/datacaching/


## Contributing

See https://github.com/memcached/memcached/wiki/DevelopmentRepos

