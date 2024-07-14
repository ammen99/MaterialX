#pragma once

#include "GLFW/glfw3.h"
#include <thread>
#include <drogon/drogon.h>
#include <iostream>
#include <json/json.h>

#define _ << " " <<
#define debug(x) #x << " = " << x

#include "Viewer.h"
#include <MaterialXGenShader/Shader.h>
#include <MaterialXRender/ShaderRenderer.h>

#include <csignal>
#include <fstream>

inline void handle_signal(int signal)
{
    std::cout << "handle signal!" << std::endl;
    nanogui::leave(); // Properly exit the NanoGUI main loop
}

inline void write_to_tmp_file(std::string filename, std::string content) {
    std::ofstream file;
    file.open(filename, std::ios::trunc | std::ios::ate);
    file << content;
    file.close();
}

inline bool set_shader_from_source(ng::ref<Viewer> viewer, std::string vertex, std::string fragment)
{
    if (auto material = viewer->getSelectedMaterial())
    {
        write_to_tmp_file("/tmp/vertex.glsl", vertex);
        write_to_tmp_file("/tmp/fragment.glsl", fragment);
        if (material->loadSource("/tmp/vertex.glsl", "/tmp/fragment.glsl", material->hasTransparency()))
        {
            try {
                material->bindShader();
                return true;
            } catch (mx::ExceptionRenderError& e) {
                std::cout << "Failed to bind shader: " << e.what() << std::endl;
                for (auto& line : e.errorLog()) {
                    std::cout << line << std::endl;
                }

                return false;
            }
        } else
        {
            std::cout << "Failed to load shader!" << std::endl;
            return false;
        }
    } else {
        std::cout << "No material selected!" << std::endl;
        return false;
    }
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
                if (auto material = viewer->getSelectedMaterial())
                {
                    r["vertex"] = material->getShader()->getSourceCode(mx::Stage::VERTEX);
                    r["fragment"] = material->getShader()->getSourceCode(mx::Stage::PIXEL);
                }

                callback(drogon::HttpResponse::newHttpJsonResponse(r));
            })

            .registerHandler("/setshader", [=] (
                    const drogon::HttpRequestPtr& _req,
                    std::function<void (const drogon::HttpResponsePtr &)> &&callback){

                auto req = _req->getJsonObject();
                if (!req) {
                    std::cout << "Invalid request: /setshader" << std::endl;
                    callback(drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                            drogon::ContentType::CT_TEXT_HTML));
                    return;
                }

                if (auto material = viewer->getSelectedMaterial())
                {
                    auto old_vertex = material->getShader()->getSourceCode(mx::Stage::VERTEX);
                    auto old_fragment = material->getShader()->getSourceCode(mx::Stage::PIXEL);

                    auto vertex = req->get("vertex", old_vertex).asString();
                    auto fragment = req->get("fragment", old_fragment).asString();

                    ng::async([=] () mutable
                    {
                        if (set_shader_from_source(viewer, vertex, fragment))
                        {
                            callback(drogon::HttpResponse::newHttpResponse());
                        } else
                        {
                            set_shader_from_source(viewer, old_vertex, old_fragment);
                            callback(drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                                    drogon::ContentType::CT_TEXT_HTML));
                        }
                    });
                }
            })

            .registerHandler("/screenshot", [viewer] (
                    const drogon::HttpRequestPtr& _req,
                    std::function<void (const drogon::HttpResponsePtr &)> &&callback) mutable {

                auto req = _req->getJsonObject();
                if (!req) {
                    std::cout << "Invalid request: /screenshot" << std::endl;
                    callback(drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                            drogon::ContentType::CT_TEXT_HTML));
                    return;
                }

                auto path = req->get("path", Json::Value("/tmp/default.png"));
                double x = req->get("x", viewer->getCameraPosition()[0]).asDouble();
                double y = req->get("y", viewer->getCameraPosition()[1]).asDouble();
                double z = req->get("z", viewer->getCameraPosition()[2]).asDouble();
                std::cout << "Pending screenshot: " << path << " x: " << x << " y: " << y << " z: " << z << " " << glfwGetTime() << std::endl;

                ng::async([=] () mutable {
                    sleep(5);
                    viewer->setCameraPosition(mx::Vector3(x, y, z));
                    viewer->requestFrameCapture(path.asString(), [=] () {
                        std::cout << "Screenshot taken: " << glfwGetTime() << std::endl;
                        callback(drogon::HttpResponse::newHttpResponse());
                    });
                });
            })

            .run();
    }

    bool main_running = true;
    std::thread server_thread;
    std::thread reset_sigint_handler;
};
