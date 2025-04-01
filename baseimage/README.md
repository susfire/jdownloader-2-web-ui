# A minimal docker baseimage to ease creation of long-lived application containers

## Requirements
- Docker

## Build Alpine Image
- docker build --network=host -f ./Dockerfile.alpine.native -t my-baseimage-alpine .

## Build Debian Image
- docker build --network=host -f ./Dockerfile.debian.native -t my-baseimage-debian .
