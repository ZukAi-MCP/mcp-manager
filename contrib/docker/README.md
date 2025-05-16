# Docker 

A couple of scripts to simplify containerization of the Smileycoin wallet.

### Build a docker image
You can build a docker image via the supplied `Dockerfile` in this way
```
# From the ./contrib/docker/ directory
docker build -f ./Dockerfile ../../ -t smileycoin/smileycoin

# From the project root directory
docker build -f ./contrib/docker/Dockerfile . -t smileycoin/smileycoin
```

### Set up a local testnet

At the time of writing there isn't a permanent testnet set up.
The `docker-compose.yml` file describes a simple set of containers, each using
a different mining algorithm. To build the containers run 
```
docker-compose build
```
followed up with 
```
docker-compose up
```
to spin up all the containers.

Once the containers are up and running, we need to connect the nodes
internally, this can be done by attaching to one container and running
`addnode` a couple of times. 

To get a list of the current docker network, run
```
docker network inspect docker_default
```

which lists all active containers in that network, including our wallet containers.

Attach to one of the containers by running
```
docker exec -it <container-id> /bin/bash
```

where `container-id` is one of the wallet containers in the list of running
containers in
```
docker ps
```

Once attached, run
```
smileycoin-cli addnode <ip-address> onetry
```
for all running wallet containers and your local testnet should be up and running.
