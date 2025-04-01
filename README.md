# Docker container for JDownloader 2 with Web User Interface

[![Donate](https://img.shields.io/badge/Donate-PayPal-green.svg?style=for-the-badge)](https://www.paypal.com/ncp/payment/46MNGVWGKCXAQ)

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