# A minimal docker baseimage to ease creation of long-lived application containers

## Requirements
- Docker
- alpine baseimage from ../baseimage-gui

## Build Image
- docker build --network=host -f ./Dockerfile -t jdownloader-2-web-ui .

## Current Version
- Alpine: alpine:3.20
- Debian: debian:12-slim
