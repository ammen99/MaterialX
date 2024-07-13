#pragma once

#include "GLFW/glfw3.h"
#include <thread>
#include <drogon/drogon.h>
#include <iostream>
#include <json/json.h>
#define _ << " " <<
#define debug(x) #x << " = " << x

#include "Viewer.h"

#include <csignal>

inline void handle_signal(int signal)
{
    std::cout << "handle signal!" << std::endl;
    nanogui::leave(); // Properly exit the NanoGUI main loop
}

/**
 * A simple class which encapsulates the state used for setting the shader and current material remotely.
 */
class Server {
  public:
    void start_server(ng::ref<Viewer> viewer, int port) {
        viewer->setFrameTiming(true);
        std::cout << "Starting HTTP Server..." << std::endl;
        server_thread = std::thread([=] () {
            this->main_loop(viewer, port);
        });
    }

    ~Server() {
        std::cout << "Stopping HTTP Server..." << std::endl;
        drogon::app().quit();
        main_running = false;
        server_thread.join();
        reset_sigint_handler.join();
    }

  private:
    void main_loop(ng::ref<Viewer> viewer, int port) {
        reset_sigint_handler = std::thread([=] () {
            while (!drogon::app().isRunning() && main_running) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }

            std::signal(SIGINT, handle_signal);
            std::signal(SIGTERM, handle_signal);
        });

        main_running = true;
        drogon::app()
            .setLogPath("./")
            .setLogLevel(trantor::Logger::kWarn)
            .addListener("0.0.0.0", port)
            .setThreadNum(1)

            .registerHandler("/getshader", [=] (
                    const drogon::HttpRequestPtr& req,
                    std::function<void (const drogon::HttpResponsePtr &)> &&callback){

                Json::Value r;
                auto [vertex, fragment] = viewer->getCurrentShaderSources();
                r["vertex"] = vertex;
                r["fragment"] = fragment;
                callback(drogon::HttpResponse::newHttpJsonResponse(r));
            })
            .registerHandler("/screenshot", [viewer] (
                    const drogon::HttpRequestPtr& _req,
                    std::function<void (const drogon::HttpResponsePtr &)> &&callback) mutable {

                auto req = _req->getJsonObject();

                auto path = req->get("path", Json::Value("/tmp/default.png"));
                double x = req->get("x", 0.0).asDouble();
                double y = req->get("y", 0.0).asDouble();
                double z = req->get("z", 5.0).asDouble();

                ng::async([=] () mutable {
                    viewer->setCameraTarget(mx::Vector3(x, y, z));
                    viewer->requestFrameCapture(path.asString());
                });

                std::cout << "Pending screenshot: " << path << " x: " << x << " y: " << y << " z: " << z << " " << glfwGetTime() << std::endl;
                viewer->waitForPendingCapture();
                std::cout << "Screenshot taken: " << glfwGetTime() << std::endl;
                callback(drogon::HttpResponse::newHttpResponse());
            })

            .run();
    }

    bool main_running = true;
    std::thread server_thread;
    std::thread reset_sigint_handler;
};
