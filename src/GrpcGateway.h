#ifndef _GRPC_GATEWAY_H
#define _GRPC_GATEWAY_H

#include <brynet/utils/app_status.h>

#include "google/protobuf/compiler/code_generator.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/compiler/importer.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/util/json_util.h"

#include "Http2Wrapper.h"

using namespace google::protobuf;
using namespace google::protobuf::compiler;

class ProxyManager
{
public:
    typedef std::shared_ptr<ProxyManager> PTR;

    ProxyManager(DiskSourceTree* sourceTree,
        AsyncConnector::PTR connector,
        std::shared_ptr<WrapTcpService> tcpService,
        std::string backendIP,
        int backendPort) :
        mImporter(sourceTree, NULL),
        mAsyncConnector(connector),
        mTcpService(tcpService),
        mBackendIP(backendIP),
        mBackendPort(backendPort)
    {}

    Importer& getImporter()
    {
        return mImporter;
    }

    const AsyncConnector::PTR& getAsyncConnector() const
    {
        return mAsyncConnector;
    }

    const std::shared_ptr<WrapTcpService>& getTcpService() const
    {
        return mTcpService;
    }

    const std::string& getBackendIP() const
    {
        return mBackendIP;
    }

    int         getBackendPort() const
    {
        return mBackendPort;
    }

private:
    Importer                        mImporter;
    AsyncConnector::PTR             mAsyncConnector;
    std::shared_ptr<WrapTcpService> mTcpService;

    std::string                     mBackendIP;
    int                             mBackendPort;
};

// receive grpc reply, translate to http, response to client
static void HandleGrpcReply(const HttpSession::PTR& httpSession,
    const TCPSession::PTR& session,
    const google::protobuf::Descriptor* outputTypeDesc,
    const uint8_t* data, 
    size_t len)
{
    DynamicMessageFactory factory;
    std::shared_ptr<google::protobuf::Message> respMsg;

    auto outPrototype = factory.GetPrototype(outputTypeDesc);
    if (outPrototype == nullptr)
    {
        goto RESPONSE_FAILED;
    }
    respMsg.reset(outPrototype->New());

    {
        BasePacketReader reader((const char*)data, len, true);
        reader.readUINT8();
        auto msgLen = reader.readUINT32();
        if (!respMsg->ParseFromArray(reader.getBuffer() + reader.getPos(), msgLen))
        {
            std::cerr << "ParseFromArray failed" << len << std::endl;
            reader.skipAll();
            goto RESPONSE_FAILED;
        }

        reader.skipAll();
    }
    {
        std::string jsonMsg;
        google::protobuf::util::MessageToJsonString(*respMsg, &jsonMsg);

        brynet::net::HttpResponse response;
        response.setBody(jsonMsg);
        response.setContentType("application/json");
        auto result = response.getResult();
        httpSession->send(result.c_str(), result.size(), [httpSession]() {
            httpSession->postShutdown();
        });
    }

    return;

RESPONSE_FAILED:
    std::cerr << "Process GRPC Reply to Client Has Error\n" << std::endl;
    httpSession->postShutdown();
}

static void HandleConnectedBackend(const ProxyManager::PTR& manager,
    const std::string& serviceName,
    const std::string& methodName,
    const std::string& requestJson,
    const HttpSession::PTR& httpSession,
    const TCPSession::PTR& session)
{
    // TODO:://now, can't use session_data for shared_ptr, it's used void* ud of some nghttp2 callback
    http2_session_data *session_data = new http2_session_data;
    if (!setupHttp2Connection(session, session_data))
    {
        goto FAILED;
    }

    do
    {
        auto serviceDesc = manager->getImporter().pool()->FindServiceByName(serviceName);
        if (serviceDesc == nullptr)
        {
            break;
        }
        auto methodDesc = serviceDesc->FindMethodByName(methodName);
        if (methodDesc == nullptr)
        {
            break;
        }
        auto inputName = methodDesc->input_type()->full_name();
        auto outputName = methodDesc->output_type()->full_name();

        auto inputTypeDesc = manager->getImporter().pool()->FindMessageTypeByName(inputName);
        auto outputTypeDesc = manager->getImporter().pool()->FindMessageTypeByName(outputName);
        if (inputTypeDesc == nullptr || outputTypeDesc == nullptr)
        {
            break;
        }

        DynamicMessageFactory factory;
        auto inputTypePrototype = factory.GetPrototype(inputTypeDesc);
        if (inputTypePrototype == nullptr)
        {
            break;
        }

        std::shared_ptr<google::protobuf::Message> requestMsg(inputTypePrototype->New());
        auto status = google::protobuf::util::JsonStringToMessage(requestJson, requestMsg.get());
        if (!status.ok())
        {
            break;
        }
        auto requestBinary = requestMsg->SerializePartialAsString();

        session->setDataCallback([session_data](const TCPSession::PTR& session, const char* buffer, size_t len) {
            auto readlen = nghttp2_session_mem_recv(session_data->session, (const uint8_t*)buffer, len);
            session_send(session_data);
            return readlen;
        });

        // TODO::desc memory manager
        auto rpcPath = std::string("/") + serviceName + "/" + methodName;
        // send grpc request to backend grpc server
        auto HandleGrpcReplyCallback = std::bind(HandleGrpcReply,
            httpSession,
            session,
            outputTypeDesc,
            std::placeholders::_1,
            std::placeholders::_2);

        sendRpcRequest(session_data, rpcPath, requestBinary, HandleGrpcReplyCallback);

        session->setDisConnectCallback([session_data](const TCPSession::PTR& session) {
            delete session_data;
        });

        // if success, eacher here
        return;
    } while (0);

FAILED:
    std::cerr << "HandleConnectedBackend Setup Http2 Has Error\n" << std::endl;
    delete session_data;
    session->postShutdown();
    httpSession->postShutdown();
}

// receive http request from client
static void HandleHttpDataFromClient(const ProxyManager::PTR& manager, const HttpSession::PTR& httpSession,
    const HTTPParser& httpParser)
{
    // you can process httpParser to control proxy

    auto queryPath = httpParser.getPath();
    std::string::size_type pos = queryPath.rfind('.');
    if (pos == std::string::npos)
    {
        return;
    }

    auto serviceName = queryPath.substr(1, pos - 1);
    auto methodName = queryPath.substr(pos + 1);
    auto body = httpParser.getBody();

    // TODO::shared grpc connection, don't connected on every client enter
    // connect backend grpc server
    manager->getAsyncConnector()->asyncConnect(manager->getBackendIP().c_str(),
        manager->getBackendPort(),
        std::chrono::seconds(10), 
        [methodNameCapture = std::move(methodName), 
        serviceNameCapture = std::move(serviceName), 
        queryPath, 
        manager,
        bodyCapture = std::move(body), 
        httpSession](TcpSocket::PTR socket) {

        auto backendSessionInitCallback = std::bind(HandleConnectedBackend,
            manager,
            std::move(serviceNameCapture), 
            std::move(methodNameCapture), 
            std::move(bodyCapture),
            httpSession, 
            std::placeholders::_1);

        manager->getTcpService()->addSession(std::move(socket),
            backendSessionInitCallback,
            false, 
            nullptr, 
            false);
    }, [httpSession]() {
        httpSession->postShutdown();
    });    
}

static void HandleClientSessionInit(const ProxyManager::PTR& manager, const TCPSession::PTR& session)
{
    // register HTTP Handle
    HttpService::setup(session, [manager](const HttpSession::PTR& httpSession) {
        auto httpDataCallback = std::bind(HandleHttpDataFromClient, manager, httpSession, std::placeholders::_1);

        //TODO:: add timeout control
        httpSession->setHttpCallback(httpDataCallback);
        httpSession->setWSCallback([=](HttpSession::PTR session, WebSocketFormat::WebSocketFrameType opcode, const std::string& payload) {
            //TODO::support websocket
        });
    });
}

// client enter
static void HaneleClientEnter(const ProxyManager::PTR& manager, brynet::net::TcpSocket::PTR socket)
{
    auto sessionInitCallback = std::bind(HandleClientSessionInit, manager, std::placeholders::_1);
    manager->getTcpService()->addSession(std::move(socket), sessionInitCallback, false, nullptr, false);
}

static void RunProxy(std::string proxyIP, int proxyPort,
    std::string backendIP, int backendPort,
    std::vector<string> protoFiles)
{
    DiskSourceTree sourceTree;
    sourceTree.MapPath("", "./");

    auto asyncConnector = AsyncConnector::Create();
    asyncConnector->startWorkerThread();
    auto tcpService = std::make_shared<WrapTcpService>();
    tcpService->startWorkThread(std::thread::hardware_concurrency());

    auto manager = std::make_shared<ProxyManager>(&sourceTree,
        asyncConnector,
        tcpService,
        backendIP,
        backendPort);

    // TODO:: use http api, receive proto content on runtime, and save to disk, then Import it.

    for (auto& file : protoFiles)
    {
        if (manager->getImporter().Import(file) == nullptr)
        {
            std::cerr << "import " << file << " failed \n" << std::endl;
            exit(-1);
        }
    }

    auto listenThread = ListenThread::Create();
    listenThread->startListen(false,
        proxyIP,
        proxyPort,
        std::bind(HaneleClientEnter, manager, std::placeholders::_1));

    while (true)
    {
        if (app_kbhit())
        {
            string input;
            std::getline(std::cin, input);

            if (input == "quit")
            {
                std::cerr << "You enter quit will exit proxy" << std::endl;
                break;
            }
        }
    }

    listenThread->stopListen();
    tcpService->stopWorkThread();
    asyncConnector->stopWorkerThread();
}

#endif