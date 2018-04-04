#include "GrpcGateway.h"

int main(int argc, char **argv)
{
    if (argc < 6)
    {
        std::cerr << "usage: proxyListenIP proxyListenPort backendIP backendPort protofile1 protofile2..." << std::endl;
        exit(-1);
    }

    std::string proxyIP = argv[3];
    int proxyPort = std::atoi(argv[4]);

    std::string backendIP = argv[1];
    int backendPort = std::atoi(argv[2]);
    
    std::vector<string> protoFiles;

    for (int i = 5; i < argc; i++)
    {
        protoFiles.push_back(argv[i]);
    }

    RunProxy(proxyIP, proxyPort, backendIP, backendPort, protoFiles);

    return 0;
}
