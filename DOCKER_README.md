# Building using Docker

Using containers can ensure a repeatable build process. The following guides you through the process.



## Prerequisites

* Docker



## Build redis-server container

```bash
docker build -t redis-ssl .
```



## Build cert generation container

**NOTE: DO NOT USE THIS ANYWHERE NEAR PRODUCTION**

```bash
docker build -t gencerts -f Dockerfile.gencerts .
```



## Build test client container

```bash
docker build -t redis-client -f Dockerfile.client .
```



## Running a test

```bash
# generate the certficates
mkdir ssl
docker run -v `pwd`/ssl:/workdir/ssl gencerts

docker network create redis
# run redis server
docker run -v `pwd`/ssl:/etc/redis/ssl --network=redis redis-ssl

# run test client.
#   NOTE: REDIS_HOST is the address of the container in the previous command
#   use the following inspect command to find it:
#   docker inspect <container ID> -f "{{ .NetworkSettings.Networks.redis.IPAddress }}"
docker run --network=redis -v `pwd`/ssl:/etc/redis/ssl --env REDIS_HOST=172.23.0.2 redis-client
```
