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

inline std::string set_shader_from_source(ng::ref<Viewer> viewer, std::string vertex, std::string fragment)
{
    if (auto material = viewer->getSelectedMaterial())
    {
        write_to_tmp_file("/tmp/vertex.glsl", vertex);
        write_to_tmp_file("/tmp/fragment.glsl", fragment);
        if (material->loadSource("/tmp/vertex.glsl", "/tmp/fragment.glsl", material->hasTransparency()))
        {
            try {
                material->bindShader();
                return "";
            } catch (mx::ExceptionRenderError& e) {
                std::cout << "Failed to bind shader: " << e.what() << std::endl;
                std::string full_error;
                for (auto& line : e.errorLog()) {
                    std::cout << line << std::endl;
                    full_error += line + "\n";
                }

                return full_error;
            }
        } else
        {
            std::cout << "Failed to load shader!" << std::endl;
            return "generic error!";
        }
    } else {
        std::cout << "No material selected!" << std::endl;
        return "invalid material state!";
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

            .registerHandler("/reset", [=] (
                    const drogon::HttpRequestPtr& req,
                    std::function<void (const drogon::HttpResponsePtr &)> &&callback) mutable {
                ng::async([=] () mutable {
                    std::cout << "Reset shader!" << std::endl;
                    viewer->getSelectedMaterial()->generateShader(viewer->getGenContext());
                    callback(drogon::HttpResponse::newHttpResponse());
                });
            })

            .registerHandler("/getshader", [=] (
                    const drogon::HttpRequestPtr& req,
                    std::function<void (const drogon::HttpResponsePtr &)> &&callback){

                ng::async([=] () mutable {
                    Json::Value r;
                    if (auto material = viewer->getSelectedMaterial())
                    {
                        r["vertex"] = material->getShader()->getSourceCode(mx::Stage::VERTEX);
                        r["fragment"] = material->getShader()->getSourceCode(mx::Stage::PIXEL);
                    }

                    callback(drogon::HttpResponse::newHttpJsonResponse(r));
                });
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
                std::cout << "Got request for set shader" << std::endl;

                ng::async([=] () mutable
                {
                    std::cout << "Dispatching request for set shader" << std::endl;

                    if (auto material = viewer->getSelectedMaterial())
                    {
                        auto old_vertex = material->getShader()->getSourceCode(mx::Stage::VERTEX);
                        auto old_fragment = material->getShader()->getSourceCode(mx::Stage::PIXEL);

                        auto vertex = req->get("vertex", old_vertex).asString();
                        auto fragment = req->get("fragment", old_fragment).asString();

                        auto error = set_shader_from_source(viewer, vertex, fragment);
                        if (error == "")
                        {
                            std::cout << "Successfully set shader!" << std::endl;
                            callback(drogon::HttpResponse::newHttpResponse());
                        } else
                        {
                            set_shader_from_source(viewer, old_vertex, old_fragment);
                            auto resp = drogon::HttpResponse::newHttpResponse(drogon::k418ImATeapot,
                                drogon::ContentType::CT_TEXT_PLAIN);
                            resp->setBody(error);
                            callback(resp);
                        }
                    } else {
                        std::cout << "Missing selected material ?!?" << std::endl;
                        auto resp = drogon::HttpResponse::newHttpResponse(drogon::k418ImATeapot,
                            drogon::ContentType::CT_TEXT_PLAIN);
                        resp->setBody("unknown error");
                        callback(resp);
                    }
                });
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

                double x = req->get("x", viewer->getCameraPosition()[0]).asDouble();
                double y = req->get("y", viewer->getCameraPosition()[1]).asDouble();
                double z = req->get("z", viewer->getCameraPosition()[2]).asDouble();
                std::cout << "Pending screenshot: " << " x: " << x << " y: " << y << " z: " << z << " " << glfwGetTime() << std::endl;

                ng::async([=] () mutable {
                    viewer->setCameraPosition(mx::Vector3(x, y, z));
                    auto img = viewer->getNextRender();

                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setContentTypeCode(drogon::CT_CUSTOM);

                    int width = img->getWidth();
                    int height = img->getHeight();
                    resp->addCookie("width", std::to_string(width));
                    resp->addCookie("height", std::to_string(height));
                    unsigned char* data = (unsigned char*)img->getResourceBuffer();
                    resp->setBody(std::string(data, data + width * height * 3));
                    callback(resp);
                    std::cout << "Screenshot taken: " << glfwGetTime() << std::endl;
                });
            })

            .registerHandler("/metrics", [=] (
                    const drogon::HttpRequestPtr& _req,
                    std::function<void (const drogon::HttpResponsePtr &)> &&callback) mutable {

                auto req = _req->getJsonObject();
                if (!req) {
                    std::cout << "Invalid request: /metrics" << std::endl;
                    callback(drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                            drogon::ContentType::CT_TEXT_HTML));
                    return;
                }

                uint width = req->get("width", 1024).asUInt();
                uint height = req->get("height", 1024).asUInt();
                uint nr_frames = req->get("frames", 100).asInt();
                uint warmup = req->get("warmup-frames", 100).asInt();

                if (width == 0 || height == 0 || width > 8192 || height > 8192) {
                    std::cout << "Malformed request: " << width << " " << height << std::endl;
                    callback(drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest, drogon::ContentType::CT_TEXT_PLAIN));
                    return;
                }

                ng::async([=] () mutable {
                    std::cout << "Running benchmark " << width << " " << height << " " << nr_frames << std::endl;
                    GLuint speed = viewer->runBenchmark(warmup, nr_frames, width, height);
                    std::cout << "Results: " << speed << std::endl;
                    Json::Value r;
                    r["speed"] = speed;
                    callback(drogon::HttpResponse::newHttpJsonResponse(r));
                });
            })
            .run();
    }

    bool main_running = true;
    std::thread server_thread;
    std::thread reset_sigint_handler;
};
