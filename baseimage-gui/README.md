# A minimal docker baseimage to ease creation of long-lived application containers

## Requirements
- Docker

## Build Alpine Image
- build alpine baseimage from ../baseimage
- docker build --network=host -f ./Dockerfile.alpine -t my-baseimage-alpine-gui .

## Build Debian Image
- build debian baseimage from ../baseimage
- docker build --network=host -f ./Dockerfile.debian -t my-baseimage-debian-gui .
