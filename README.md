# Memcached with Shared Memory
Project Members: Xing Li, Moumita Samanta, Saumya Solanki

For the original implementation of memcached which this is forked from please refer the the github link below:
https://github.com/memcached/memcached

## Dependencies
* libevent, http://www.monkey.org/~provos/libevent/ (libevent-dev)
* librt, in order to allow the use of POSIX shared memory

## Changes and Motivation
Implemented a malloc command that allocates from a shared pool or memory reservered only for memcached instances. 
This allows for a global view of memcached information and allows for future optimizations and more efficient slab allocation algorithms


## Setup
To compile memcached from the github repo 
* Run ./autogen.sh then ./configure
* Make sure you have automake, autotools-dev, and autoconf installed on your linux system (it's included in the docker image)
* Edit the LIBS in the generated Makefile to include -lrt (should already have -levent)
* Compile init_share and stop_share (i.e. gcc -o init_share init_share.c)
* First run ./init_share with the total amount of shared memory you need allocated. Then run memcached with the default parameters (see memcached github for specifics)
* Once finished run ./stop_share to unlink the shared memory segement

## Setup Notes
Make sure that the /dev/shm is large enough to host your shared memory, if not, you must edit /etc/fstab with the following parameters to the first line (if it's empty, otherwise edit the line containing /dev/shm)
* tmpfs tmpfs      /dev/shm      tmpfs   defaults,size=2g   0   0

Modify the size to the size necessary to fit the amount of shared memory required then execute
```
mount -o remount mount
```

For you to have the nccessary permissions, you might have to disable applocker if using an Ubuntu distro. For the docker image, start it with the following command:
```
docker run  --name dc-server --net caching_network --cap-add SYS_ADMIN --security-opt apparmor:unconfined -d aznmonkey/data-caching:server -t 1
```

To run the client, follow the documentation from Cloudsuite:
http://cloudsuite.ch///pages/benchmarks/datacaching/

## Environment
Any Linux based system should work but if you need working docker images they are listed below:
https://hub.docker.com/r/aznmonkey/data-caching/

There are two flags, server and client. The server initiates the memcached server itself and the client initialiates the client for performance evaluation, they are based on the client and server from Cloudsuite. 


## Contributing

See https://github.com/memcached/memcached/wiki/DevelopmentRepos
