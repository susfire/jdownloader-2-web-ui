#!/bin/bash

docker build --network=host -f baseimage/Dockerfile.alpine.native -t my-baseimage-alpine .
docker build --network=host -f baseimage-gui/Dockerfile.alpine -t my-baseimage-alpine-gui .
docker build --network=host -f jdownloader-2-web-ui/Dockerfile -t jdownloader-2-web-ui .