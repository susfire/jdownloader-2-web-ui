# Docker container for JDownloader 2 with Web User Interface

[![Donate](https://img.shields.io/badge/Donate-PayPal-green.svg?style=for-the-badge)](https://www.paypal.com/ncp/payment/46MNGVWGKCXAQ)

## About
A project forked from jlesage/docker-jdownloader-2 with a slightly different approach. I prefer Dockerfiles being built in a single project and being able to build them myself locally from base images such as alpine, debian, node, etc. Therefore I took the Dockerfiles and modified them to be able to build them myself. If anyone needs a self-hosted jdownloader 2 such as myself, I hope you take joy in my approach.

## Requirements
- Docker

## Build Images
- docker build --network=host -f baseimage/Dockerfile.alpine.native -t my-baseimage-alpine .
- docker build --network=host -f baseimage-gui/Dockerfile.alpine -t my-baseimage-alpine-gui .
- docker build --network=host -f jdownloader-2-web-ui/Dockerfile -t jdownloader-2-web-ui .

or convenient script:

- chmod a+x ./build-image.sh && ./build-image.sh

## Start Container
- docker compose up -d

## Want to buy me a coffee?
- https://www.paypal.com/ncp/payment/46MNGVWGKCXAQ