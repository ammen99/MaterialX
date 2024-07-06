#pragma once

#include <thread>
#include <drogon/drogon.h>
#include <iostream>
#define _ << " " <<
#define debug(x) #x << " = " << x

/**
 * A simple class which encapsulates the state used for setting the shader and current material remotely.
 */
class Server {
  public:
    void start_server(int port) {
        std::cout << "Starting HTTP Server..." << std::endl;
        server_thread = std::thread([=] () {
            this->main_loop(port);
        });
    }

    ~Server() {
        std::cout << "Stopping HTTP Server..." << std::endl;
        drogon::app().quit();
        server_thread.join();
    }

  private:
    void main_loop(int port) {
        drogon::app()
            .setLogPath("./")
            .setLogLevel(trantor::Logger::kWarn)
            .addListener("0.0.0.0", port)
            .setThreadNum(1)

            .registerHandler("/getshader", [=] (
                    const drogon::HttpRequestPtr& req,
                    std::function<void (const drogon::HttpResponsePtr &)> &&callback){

                auto response = drogon::HttpResponse::newHttpResponse();
                response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                response->setBody("Hello World!");
                callback(response);

            })

            .run();
    }

    std::thread server_thread;
};
