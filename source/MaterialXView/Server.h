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
#include <MaterialXRenderGlsl/GlslMaterial.h>

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

inline Json::Value vecUniformToJson(std::string name, int dimensions, float min, float max, std::string hint)
{
    Json::Value v;
    v["name"] = name;
    v["type"] = "vec";
    v["dimensions"] = dimensions;
    v["min"] = min;
    v["max"] = max;
    v["hint"] = hint;
    return v;
}

inline Json::Value boolUniformToJson(std::string name)
{
    Json::Value v;
    v["name"] = name;
    v["type"] = "boolean";
    return v;
}

class ServerController : public drogon::HttpController<ServerController, false>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ServerController::reset, "/reset");
    ADD_METHOD_TO(ServerController::getshader, "/getshader");
    ADD_METHOD_TO(ServerController::getuniforms, "/getuniforms");
    ADD_METHOD_TO(ServerController::setuniforms, "/setuniforms");
    ADD_METHOD_TO(ServerController::metrics, "/metrics");
    ADD_METHOD_TO(ServerController::setshader, "/setshader");
    ADD_METHOD_TO(ServerController::screenshot, "/screenshot");
    METHOD_LIST_END

    struct CachedShader {
        std::string vertex;
        std::string fragment;
        mx::GlslMaterialPtr cache;
        CachedShader() {
            cache = mx::GlslMaterial::create();
        }
    };

    std::array<CachedShader, 2> cachedShaders;
    static constexpr size_t CACHE_DEFAULT = 0;
    static constexpr size_t CACHE_TMP = 1;

    void storeProgramInCache(mx::GlslMaterialPtr material, size_t idx,
        std::string vertex = "", std::string fragment = "")
    {
        if (vertex == "") {
            vertex = material->getProgram()->getShader()->getSourceCode(mx::Stage::VERTEX);
        }

        if (fragment == "") {
            fragment = material->getProgram()->getShader()->getSourceCode(mx::Stage::PIXEL);
        }

        std::cout << "Storing " << (idx == CACHE_DEFAULT ? "default" : "tmp") << " " << vertex.length() << " " << fragment.length() << std::endl;

        cachedShaders[idx].vertex = vertex;
        cachedShaders[idx].fragment = fragment;
        cachedShaders[idx].cache->copyShader(material);
    }

    void setProgramFromCache(mx::GlslMaterialPtr material, CachedShader *shader)
    {
        std::cout << "Reusing cache entry " << (shader == &cachedShaders[CACHE_DEFAULT] ? "default" : "tmp") << std::endl;
        material->copyShader(shader->cache);
        viewer->assignMaterial(viewer->getSelectedGeometry(), material, false);
    }

    std::string setShaderFromSource(ng::ref<Viewer> viewer, std::string vertex, std::string fragment)
    {
        auto material = std::dynamic_pointer_cast<mx::GlslMaterial>(viewer->getSelectedMaterial());
        if (!material)
        {
            std::cout << "No material selected!" << std::endl;
            return "invalid material state!";
        }

        for (auto& cache: cachedShaders)
        {
            if (cache.vertex == vertex && cache.fragment == fragment)
            {
                setProgramFromCache(material, &cache);
                return "";
            }
        }

        write_to_tmp_file("/tmp/vertex.glsl", vertex);
        write_to_tmp_file("/tmp/fragment.glsl", fragment);
        if (material->loadSource("/tmp/vertex.glsl", "/tmp/fragment.glsl", material->hasTransparency()))
        {
            try {
                material->bindShader();
                storeProgramInCache(material, CACHE_TMP, vertex, fragment);
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
    }

    void reset(const drogon::HttpRequestPtr& req,
        std::function<void (const drogon::HttpResponsePtr &)> &&callback)
    {
        ng::async([=] () mutable {
            std::cout << "Reset shader!" << std::endl;

            auto material = std::dynamic_pointer_cast<mx::GlslMaterial>(viewer->getSelectedMaterial());
            //if (cachedShaders[CACHE_DEFAULT].vertex.empty()) {
                material->generateShader(viewer->getGenContext());
                material->bindShader();
                std::cout << "generating anew " << material->hasTransparency() << std::endl;
                storeProgramInCache(material, CACHE_DEFAULT);
            //} else {
            //    setProgramFromCache(material, &cachedShaders[CACHE_DEFAULT]);
            //}

            callback(drogon::HttpResponse::newHttpResponse());
        });
    }

    void getshader(const drogon::HttpRequestPtr& req,
        std::function<void (const drogon::HttpResponsePtr &)> &&callback)
    {
        ng::async([=] () mutable {
            Json::Value r;
            if (auto material = viewer->getSelectedMaterial())
            {
                r["vertex"] = material->getShader()->getSourceCode(mx::Stage::VERTEX);
                r["fragment"] = material->getShader()->getSourceCode(mx::Stage::PIXEL);
            }

            callback(drogon::HttpResponse::newHttpJsonResponse(r));
        });
    }

    void append_uniform(Json::Value& list, const mx::UIPropertyItem& item)
    {
        auto value = item.variable->getValue();

        if (value->getTypeString() == "float")
        {
            // TODO: is this meaningful?
            float min = -10;
            float max = 10;
            if (item.ui.uiMin)
                min = item.ui.uiMin->asA<float>();
            if (item.ui.uiMax)
                max = item.ui.uiMax->asA<float>();
            list.append(vecUniformToJson(item.variable->getPath(), 1, min, max, "float"));
        }
        else if (value->getTypeString() == "color3")
        {
            list.append(vecUniformToJson(item.variable->getPath(), 3, 0, 1, "color3"));
        }
        else if (value->getTypeString() == "boolean")
        {
            list.append(boolUniformToJson(item.variable->getPath()));
        } else
        {
            std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! Unknown type: " << value->getTypeString() << std::endl;
        }

        std::cout << debug(item.variable->getPath()) _ debug(value->getTypeString()) << std::endl;
    }

    std::optional<std::string> setValue(mx::MaterialPtr material, Json::Value& uniformValue, mx::ShaderPort *uniform)
    {
        auto path = uniformValue["name"].asString();
        auto value = uniformValue["value"];

        if (uniform->getValue()->getTypeString() == "float")
        {
            if (!value.isArray() || value.size() != 1 || !value[0].isDouble())
            {
                return "Invalid float value for " + path;
            }

            material->modifyUniform(path, mx::Value::createValue(float(value[0].asDouble())));
        }
        else if (uniform->getValue()->getTypeString() == "color3")
        {
            if (!value.isArray() || value.size() != 3 || !value[0].isDouble() || !value[1].isDouble() || !value[2].isDouble())
            {
                return "Invalid color value for " + path;
            }

            material->modifyUniform(path, mx::Value::createValue(
                    mx::Color3(value[0].asDouble(), value[1].asDouble(), value[2].asDouble())));
        }
        else if (uniform->getValue()->getTypeString() == "boolean")
        {
            if (!value.isBool())
            {
                return "Invalid boolean value for " + path;
            }

            material->modifyUniform(path, mx::Value::createValue(value.asBool()));
        } else
        {
            std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! Unknown type for set: " << uniform->getValue()->getTypeString() << std::endl;
        }

        return {};
    }

    void getuniforms(const drogon::HttpRequestPtr& req,
        std::function<void (const drogon::HttpResponsePtr &)> &&callback)
    {
        ng::async([this, callback] () mutable {
            Json::Value r = Json::arrayValue;

            r.append(vecUniformToJson("camera", 3, -5, 5, "camera"));

            if (auto material = viewer->getSelectedMaterial())
            {
                if (material->getPublicUniforms())
                {
                    auto& uniforms = *material->getPublicUniforms();

                    mx::UIPropertyGroup groups;
                    mx::UIPropertyGroup unnamedGroups;
                    const std::string pathSeparator(":");
                    mx::createUIPropertyGroups(material->getElement()->getDocument(),
                        uniforms, groups, unnamedGroups, pathSeparator);

                    for (auto uniform : groups)
                    {
                        append_uniform(r, uniform.second);
                    }
                    for (auto uniform : unnamedGroups)
                    {
                        append_uniform(r, uniform.second);
                    }
                }
            }

            callback(drogon::HttpResponse::newHttpJsonResponse(r));
        });
    }

    void setuniforms(const drogon::HttpRequestPtr& _req,
        std::function<void (const drogon::HttpResponsePtr &)> &&callback)
    {
        auto req = _req->getJsonObject();
        if (!req || !req->isArray()) {
            std::cout << "Invalid request: /setuniforms: expected json array" << std::endl;
            callback(drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                    drogon::ContentType::CT_TEXT_HTML));
            return;
        }

        ng::async([this, callback, req] () mutable {
            auto material = std::dynamic_pointer_cast<mx::GlslMaterial>(viewer->getSelectedMaterial());
            if (!material)
            {
                return;
            }


            for (auto it = req->begin(); it != req->end(); it++) {
                auto uniformValue = *it;
                if (!uniformValue.isObject() ||
                    !uniformValue.isMember("name") || !uniformValue["name"].isString() ||
                    !uniformValue.isMember("value"))
                {
                    auto resp = drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                            drogon::ContentType::CT_TEXT_HTML);
                    resp->setBody("Invalid request: wrong syntax (expected array of name and value)");
                    callback(resp);
                    return;
                }

                auto name = uniformValue["name"].asString();
                if (name == "camera")
                {
                    if (!uniformValue["value"].isArray() || uniformValue["value"].size() != 3
                        || !uniformValue["value"][0].isDouble() || !uniformValue["value"][1].isDouble() || !uniformValue["value"][2].isDouble())
                    {
                        auto resp = drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                                drogon::ContentType::CT_TEXT_HTML);
                        resp->setBody("Invalid request: wrong syntax (expected array of name and value)");
                        callback(resp);
                        return;
                    }

                    viewer->setCameraPosition(mx::Vector3(
                            uniformValue["value"][0].asDouble(), uniformValue["value"][1].asDouble(), uniformValue["value"][2].asDouble()));
                }

                if (auto uniform = material->findUniform(name))
                {
                    if (auto err = setValue(material, uniformValue, uniform))
                    {
                        auto resp = drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                                drogon::ContentType::CT_TEXT_HTML);
                        resp->setBody(err.value());
                        callback(resp);
                        return;
                    }
                }
            }

            std::dynamic_pointer_cast<mx::GlslMaterial>(material)->updateTransparency(viewer->getGenContext());
            std::cout << "Material now has transparency $$$$$$$$$$$$$$$$$$ " << material->hasTransparency() << std::endl;
            callback(drogon::HttpResponse::newHttpResponse());
        });
    }

    void setshader(const drogon::HttpRequestPtr& _req,
        std::function<void (const drogon::HttpResponsePtr &)> &&callback)
    {
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

                auto error = setShaderFromSource(viewer, vertex, fragment);
                if (error == "")
                {
                    std::cout << "Successfully set shader!" << std::endl;
                    callback(drogon::HttpResponse::newHttpResponse());
                } else
                {
                    setShaderFromSource(viewer, old_vertex, old_fragment);
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
    }

    void screenshot(const drogon::HttpRequestPtr& _req,
        std::function<void (const drogon::HttpResponsePtr &)> &&callback)
    {
        auto req = _req->getJsonObject();
        if (!req) {
            std::cout << "Invalid request: /screenshot" << std::endl;
            callback(drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                    drogon::ContentType::CT_TEXT_HTML));
            return;
        }

        int w = req->get("width", viewer->width()).asInt();
        int h = req->get("height", viewer->height()).asInt();
        std::cout << "Pending screenshot: " << " width=" << w << " height=" << h << " " << glfwGetTime() << std::endl;

        ng::async([=] () mutable {
            auto img = viewer->getNextRender(w, h);

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
    }

    void metrics(const drogon::HttpRequestPtr& _req,
            std::function<void (const drogon::HttpResponsePtr &)> &&callback) {

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
    }

    ng::ref<Viewer> viewer;
    ServerController(ng::ref<Viewer> viewer) : viewer(viewer) {}
};



/**
 * A simple class which encapsulates the state used for setting the shader and current material remotely.
 */
class Server {
  public:
    void start_server(ng::ref<Viewer> viewer, int port) {
        viewer->setFrameTiming(true);
        std::cout << "Starting HTTP Server... at port=" << port << std::endl;
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
            .registerController(std::make_shared<ServerController>(viewer))
            .run();
    }

    bool main_running = true;
    std::thread server_thread;
    std::thread reset_sigint_handler;
};
