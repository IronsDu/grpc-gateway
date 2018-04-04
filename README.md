# grpc-gateway
The GRPC HTTP Proxy

## Features
* not need modify .proto
* not need generate code before compiler 

## Dependent
* [nghttp2](https://github.com/nghttp2/nghttp2)
* [protobuf](https://github.com/google/protobuf)
* [brynet](https://github.com/IronsDu/brynet)

## Build On Windows
1. install [vcpkg](https://github.com/Microsoft/vcpkg)
2. `vcpkg install protobuf`
3. `vcpkg install nghtt2`
4. `vcpkg install brynet --head`
5. mkdir build & cd build
6. cmake ..

## Build On Linux
Please install protobuf,nghtt2,brynet by hand.

## Test
1. run your grpc server, listen port:`8888`
2. run your proxy of cmd: `grpc_proxy.exe 0.0.0.0 8080 127.0.0.1 8888 echo_service.proto`
3. use `curl -v -XPOST http://localhost:8080/sofa.pbrpc.test.EchoServer.Echo -d {"message":"hello"}`

## TODO
* Dont't create new connection to grpc server when every client enter.
* Use some internal HTTP API for control proxy server, eg compiler .proto from client runtime.
even for reload .proto.