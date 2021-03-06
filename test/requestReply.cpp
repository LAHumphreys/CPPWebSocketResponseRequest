#include "gtest/gtest.h"

#include "WorkerThread.h"
#include "ReqServer.h"
#include "io_thread.h"
#include "ClientUtils.h"

const size_t serverPort = 1250;
const std::string serverUri = "ws://localhost:1250";

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

class EchoSvr: public RequestReplyHandler
{
public:
    static constexpr const char* REQUEST_TYPE = "ECHO";

    virtual std::string OnRequest(const char* request) {
        return request;
    }
    static std::unique_ptr<RequestReplyHandler> New() {
        return std::make_unique<EchoSvr>();
    }
};

class RejectSvr: public RequestReplyHandler
{
public:
    static constexpr const char* REQUEST_TYPE = "REJECT";
    static const int ERROR_CODE;

    virtual std::string OnRequest(const char* request) {
        throw InvalidRequestException {ERROR_CODE, request};
    }

    static std::unique_ptr<RequestReplyHandler> New() {
        return std::make_unique<RejectSvr>();
    }
};
const int RejectSvr::ERROR_CODE = 12;

class PauseSvr: public RequestReplyHandler
{
public:
    PauseSvr(std::promise<bool>& goFlag) : go(goFlag.get_future()) {}

    static constexpr const char* REQUEST_TYPE = "PAUSE";

    virtual std::string OnRequest(const char* request) {
        go.wait();
        return request;
    }
    static std::unique_ptr<RequestReplyHandler> New(std::promise<bool>& goFlag) {
        return std::make_unique<PauseSvr>(goFlag);
    }

    std::future<bool> go;
};

class CrashSvr: public RequestReplyHandler
{
public:
    static constexpr const char* REQUEST_TYPE = "CRASH";

    struct CrashError: FatalError {};

    virtual std::string OnRequest(const char* request) {
        throw CrashError {};
    }
    static std::unique_ptr<RequestReplyHandler> New() {
        return std::make_unique<CrashSvr>();
    }
};

// Positive test - send a request, get a response...
TEST(REQ_CLIENT, SuccessfulRequest)
{
    WorkerThread serverThread;
    RequestServer server;
    IOThread clientThread;

    // Request server's main loop is blocking, start up on a slave thread...
    serverThread.PostTask([&] () -> void {
        server.AddHandler(EchoSvr::REQUEST_TYPE, EchoSvr::New());
        server.HandleRequests(serverPort);
    });
    serverThread.Start();

    // wait for the server to spin up...
    server.WaitUntilRunning();

    // Ok we're all set - trigger the request
    const std::string payload = "Hello World!";
    auto request =
            clientThread.Request(serverUri, EchoSvr::REQUEST_TYPE, payload);

    // Request is being managed on the IO Thread - wait for it to retrieve the
    // response and then check it got the right value...
    auto response = request->WaitForMessage();
    ASSERT_EQ(response.content_, payload);
    ASSERT_EQ(response.state_, ReplyMessage::COMPLETE);
}


TEST(REQ_CLIENT, ForceShutdown)
{
    WorkerThread serverThread;
    RequestServer server;
    IOThread clientThread;

    std::promise<bool> stopFlag;
    auto futureStop = stopFlag.get_future();

    // Request server's main loop is blocking, start up on a slave thread...
    serverThread.PostTask([&] () -> void {
        server.HandleRequests(serverPort);
        stopFlag.set_value(true);
    });
    serverThread.Start();

    // wait for the server to spin up...
    server.WaitUntilRunning();

    // Ok we're all set - trigger the request
    const std::string payload = "Hello World!";
    auto request =
            clientThread.Request(serverUri, "stop-listening", "");

    ASSERT_TRUE(futureStop.get());
}

// Handle legitmate rejects from the server
TEST(REQ_CLIENT, RejectedRequest)
{
    WorkerThread serverThread;
    RequestServer server;
    IOThread clientThread;

    // Request server's main loop is blocking, start up on a slave thread...
    serverThread.PostTask([&] () -> void {
        server.AddHandler(RejectSvr::REQUEST_TYPE, RejectSvr::New());
        server.HandleRequests(serverPort);
    });
    serverThread.Start();

    // wait for the server to spin up...
    server.WaitUntilRunning();

    // Ok we're all set - trigger the request
    const std::string payload = "Hello World!";
    auto request =
            clientThread.Request(serverUri, RejectSvr::REQUEST_TYPE, payload);

    // Request is being managed on the IO Thread - wait for it to retrieve the
    // response and then check it got the right value...
    auto response = request->WaitForMessage();
    ASSERT_EQ(response.state_, ReplyMessage::REJECTED);
    ASSERT_EQ(response.code_, 12);
    ASSERT_EQ(response.content_, payload);
}

// Handle malformed rejectes from the server
TEST(REQ_CLIENT, MalformedReject)
{
    WorkerThread serverThread;
    RequestServer server;
    IOThread clientThread;

    // Request server's main loop is blocking, start up on a slave thread...
    serverThread.PostTask([&] () -> void {
        server.AddHandler(EchoSvr::REQUEST_TYPE, EchoSvr::New());
        server.HandleRequests(serverPort);
    });
    serverThread.Start();

    // wait for the server to spin up...
    server.WaitUntilRunning();

    // Ok we're all set - trigger the request
    const std::string payload = "ERROR Invalid param 'fishy'";

    auto request =
            clientThread.Request(serverUri, EchoSvr::REQUEST_TYPE, payload);

    // Request is being managed on the IO Thread - wait for it to retrieve the
    // response and then check it got the right value...
    auto response = request->WaitForMessage();

    // By definition a mal-formed reject is not actually a reject at all, just
    // an vanilla reply
    ASSERT_EQ(response.state_, ReplyMessage::COMPLETE);
    ASSERT_EQ(response.code_, 0);
    ASSERT_EQ(response.content_, payload);
}

// A sensible error_ is thrown if the request URI is garbage
TEST(REQ_CLIENT, InvalidURI)
{
    IOThread clientThread;

    const std::string payload = "Hello World!";
    auto request =
            clientThread.Request("NOT_A_VALID URI", EchoSvr::REQUEST_TYPE, payload);

    // Request is being managed on the IO Thread - wait for it to retrieve the
    // response and then check it got the right value...
    ASSERT_THROW(request->WaitForMessage(), ReqSvrRequest::InvalidURIError);
}

// A sensible error_ is thrown if the server cannot be contacted
TEST(REQ_CLIENT, ServerDown)
{
    IOThread clientThread;

    const std::string payload = "Hello World!";
    auto request =
            clientThread.Request(serverUri, EchoSvr::REQUEST_TYPE, payload);

    // Request is being managed on the IO Thread - wait for it to retrieve the
    // response and then check it got the right value...
    ASSERT_THROW(
            request->WaitForMessage(),
            ReqSvrRequest::ServerDisconnectedError);
}

// Validate the system will not crash / behave badly if the request handle is
// abandoned.
TEST(REQ_CLIENT, AbandonedRequest)
{
    WorkerThread serverThread;
    RequestServer server;
    IOThread clientThread;

    std::promise<bool> goFlag;

    // Request server's main loop is blocking, start up on a slave thread...
    serverThread.PostTask([&] () -> void {
        server.AddHandler(PauseSvr::REQUEST_TYPE, PauseSvr::New(goFlag));
        server.HandleRequests(serverPort);
    });
    serverThread.Start();

    // wait for the server to spin up...
    server.WaitUntilRunning();

    // Ok we're all set - trigger the request
    const std::string payload = "Hello World!";

    // Throw away the request handle
    clientThread.Request(serverUri, PauseSvr::REQUEST_TYPE, payload);

    // now resume the server...
    goFlag.set_value(true);

    std::promise<bool> doneFlag;

    // and wait for it to terminate...
    server.PostTask( [&] () {
        doneFlag.set_value(true);
    });
    doneFlag.get_future().wait();
}

/*
 * Handle server failure during request processing
 */
TEST(REQ_CLIENT, InFlightDisconnect)
{
    WorkerThread serverThread;
    IOThread clientThread;

    std::promise<bool> goFlag;

    // Request server's main loop is blocking, start up on a slave thread...
    serverThread.PostTask([&] () -> void {
        RequestServer server;
        server.AddHandler(CrashSvr::REQUEST_TYPE, CrashSvr::New());
        server.PostTask([&] () -> void {
            goFlag.set_value(true);
        });
        ASSERT_THROW(
                server.HandleRequests(serverPort),
                RequestServer::FatalErrorException);
    });
    serverThread.Start();

    // wait for the server to spin up...
    goFlag.get_future().wait();

    // Ok we're all set - trigger the request
    const std::string payload = "Hello World!";
    auto request =
            clientThread.Request(serverUri, CrashSvr::REQUEST_TYPE, payload);

    ASSERT_THROW(
            request->WaitForMessage(),
            ReqSvrRequest::ServerDisconnectedError);

}

/**
 * Ensure we can't have two active servers listening on the same port...
 */
TEST(REQ_CLIENT, NoDoublePortBind)
{
    WorkerThread serverThread;
    RequestServer server;

    // Request server's main loop is blocking, start up on a slave thread...
    serverThread.PostTask([&] () -> void {
        server.AddHandler(EchoSvr::REQUEST_TYPE, EchoSvr::New());
        server.HandleRequests(serverPort);
    });
    serverThread.Start();

    server.WaitUntilRunning();

    // Ok, now attempt to spin up a second server. It should be rejected on the
    // grounds of a double port acquisition
    RequestServer duplicate;
    duplicate.AddHandler(EchoSvr::REQUEST_TYPE, EchoSvr::New());
    ASSERT_THROW(
        duplicate.HandleRequests(serverPort),
        RequestServer::FatalErrorException);
}

TEST(FILE_CLIENT, FileRequest)
{
    WorkerThread serverThread;
    RequestServer server;
    // Request server's main loop is blocking, start up on a slave thread...
    serverThread.PostTask([&] () -> void {
        server.AddHandler(EchoSvr::REQUEST_TYPE, EchoSvr::New());
        server.HandleRequests(serverPort);
    });
    serverThread.Start();

    // wait for the server to spin up...
    server.WaitUntilRunning();

    std::stringstream sport;
    sport << serverPort;
    std::string result = ClientUtils::FileRequestLocal(sport.str(), EchoSvr::REQUEST_TYPE, "helloWorld.msg");

    ASSERT_EQ(result, "Hello World!");
}

TEST(JSON_CLIENT, JSONRequest)
{
    WorkerThread serverThread;
    RequestServer server;
    // Request server's main loop is blocking, start up on a slave thread...
    serverThread.PostTask([&] () -> void {
        server.AddHandler(EchoSvr::REQUEST_TYPE, EchoSvr::New());
        server.HandleRequests(serverPort);
    });
    serverThread.Start();

    // wait for the server to spin up...
    server.WaitUntilRunning();

    std::stringstream sport;
    sport << serverPort;
    std::map<std::string, std::string> values = {
            {"msg", "Hello World!"}
    };
    const std::string JSONMsg = R"RAW({"msg":"Hello World!"})RAW";
    std::string result = ClientUtils::JSONRequestLocal(sport.str(), EchoSvr::REQUEST_TYPE, values);

    ASSERT_EQ(result, JSONMsg);
}

TEST(FILE_CLIENT, RejectedRequest)
{
    WorkerThread serverThread;
    RequestServer server;
    // Request server's main loop is blocking, start up on a slave thread...
    serverThread.PostTask([&] () -> void {
        server.AddHandler(EchoSvr::REQUEST_TYPE, RejectSvr::New());
        server.HandleRequests(serverPort);
    });
    serverThread.Start();

    // wait for the server to spin up...
    server.WaitUntilRunning();

    std::stringstream sport;
    sport << serverPort;
    std::string result = ClientUtils::FileRequestLocal(sport.str(), EchoSvr::REQUEST_TYPE, "helloWorld.msg");

    std::stringstream expected;
    expected << "[REJECTED] Server Rejected our request: " << std::endl
             << "    " << "Server URI: ws://localhost:1250" << std::endl
             << "    " << "Code:       " << RejectSvr::ERROR_CODE << std::endl
             << "    " << "Error:      " << "Hello World!" << std::endl;

    ASSERT_EQ(result, expected.str());

}

TEST(FILE_CLIENT, BadFile)
{
    WorkerThread serverThread;
    RequestServer server;
    // Request server's main loop is blocking, start up on a slave thread...
    serverThread.PostTask([&] () -> void {
        server.AddHandler(EchoSvr::REQUEST_TYPE, EchoSvr::New());
        server.HandleRequests(serverPort);
    });
    serverThread.Start();

    // wait for the server to spin up...
    server.WaitUntilRunning();

    std::stringstream sport;
    sport << serverPort;
    std::string result = ClientUtils::FileRequestLocal(sport.str(), EchoSvr::REQUEST_TYPE, "Not a file");

    ASSERT_EQ(result, "[BAD_FILE] Not a file");
}

TEST(FILE_CLIENT, BadPort)
{
    WorkerThread serverThread;
    RequestServer server;
    // Request server's main loop is blocking, start up on a slave thread...
    serverThread.PostTask([&] () -> void {
        server.AddHandler(EchoSvr::REQUEST_TYPE, EchoSvr::New());
        server.HandleRequests(serverPort);
    });
    serverThread.Start();

    // wait for the server to spin up...
    server.WaitUntilRunning();

    std::stringstream sport;
    sport << (serverPort + 5);
    std::string result = ClientUtils::FileRequestLocal(sport.str(), EchoSvr::REQUEST_TYPE, "helloWorld.msg");

    ASSERT_EQ(result.substr(0, 12), "[DISCONNECT]");
}

TEST(FILE_CLIENT, NonIntegerPort)
{
    WorkerThread serverThread;
    RequestServer server;
    // Request server's main loop is blocking, start up on a slave thread...
    serverThread.PostTask([&] () -> void {
        server.AddHandler(EchoSvr::REQUEST_TYPE, EchoSvr::New());
        server.HandleRequests(serverPort);
    });
    serverThread.Start();

    // wait for the server to spin up...
    server.WaitUntilRunning();

    std::string result = ClientUtils::FileRequestLocal("Not A Port", EchoSvr::REQUEST_TYPE, "helloWorld.msg");

    ASSERT_EQ(result.substr(0, 13), "[INVALID_URI]");
}
