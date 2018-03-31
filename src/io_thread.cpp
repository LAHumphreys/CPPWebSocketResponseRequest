#include "io_thread.h"
#include <iostream>
#include <logger.h>

IOThread::IOThread()
   : io_thread(&IOThread::IOLoop, this)
   , requestClient(io_service)
{
}

IOThread::~IOThread() {
    Stop();
}
void IOThread::Stop() {
    io_service.stop();
    io_thread.join();
}

std::shared_ptr<HTTPRequest> IOThread::HTTPSRequest(
    const std::string& server,
    const std::string& path)
{
    std::shared_ptr<HTTPRequest> req(
       new HTTPRequest(io_service,server,path));

    return req;
}

std::shared_ptr<HTTPRequest> IOThread::HTTPSPOST(
    const std::string& server,
    const std::string& path,
    const std::string& data,
    const AsyncHTTPSClient::HeaderMap& headers)
{
    std::shared_ptr<HTTPRequest> req(
       new HTTPRequest(io_service,server,path,data,headers));

    return req;
}

std::shared_ptr<ReqSvrRequest> IOThread::Request(
        const std::string& uri,
        const std::string& requestName,
        const std::string& jsonData)
{
    auto req =
        std::make_shared<ReqSvrRequest>(requestName, jsonData);

    PostTask([=] () -> void {
        this->requestClient.newConnection(uri, req);
    });

    return req;
}

void IOThread::IOLoop() {
    boost::asio::io_service::work work(io_service);
    io_service.run();
}

void IOThread::PostTask(const IPostable::Task &t) {
    io_service.post(t);
}
     