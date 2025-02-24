#pragma once

#include "GLFW/glfw3.h"
#include <thread>
#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <iostream>
#include <json/json.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define _ << " " <<
#define debug(x) #x << " = " << x

#include "Viewer.h"
#include <MaterialXGenShader/Shader.h>
#include <MaterialXRender/ShaderRenderer.h>
#include <MaterialXRenderGlsl/GlslMaterial.h>

#include <csignal>

inline void handle_signal(int signal)
{
    std::cout << "handle signal!" << std::endl;
    nanogui::leave(); // Properly exit the NanoGUI main loop
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
        mx::GlslProgramPtr cache;
        CachedShader() { }
    };

    struct ProgramCache {
        static constexpr size_t CACHE_DEFAULT = 0;
        std::array<CachedShader, 3> cachedShaders;
        size_t next_tmp_cache = 1;

        void storeProgramInCache(mx::GlslMaterialPtr material, bool isDefault,
            std::string vertex = "", std::string fragment = "")
        {
            size_t idx = isDefault ? CACHE_DEFAULT : next_tmp_cache;
            if (!isDefault) {
                // toggle between indices 1 and 2
                next_tmp_cache = 3 - next_tmp_cache;
            }

            if (vertex == "") {
                vertex = material->getProgram()->getShader()->getSourceCode(mx::Stage::VERTEX);
            }

            if (fragment == "") {
                fragment = material->getProgram()->getShader()->getSourceCode(mx::Stage::PIXEL);
            }

            std::cout << "Storing " <<
                (idx == CACHE_DEFAULT ? "default" : "tmp") << " " << vertex.length() << " " << fragment.length() << std::endl;

            cachedShaders[idx].vertex = vertex;
            cachedShaders[idx].fragment = fragment;
            cachedShaders[idx].cache = material->getProgram();
        }

        mx::GlslProgramPtr findProgram(std::string vertex, std::string fragment)
        {
            for (auto& cache: cachedShaders)
            {
                if (cache.vertex == vertex && cache.fragment == fragment)
                {
                    return cache.cache;
                }
            }

            return nullptr;
        }

        mx::GlslProgramPtr& getDefault() {
            return cachedShaders[CACHE_DEFAULT].cache;
        }
    };

    std::map<mx::MaterialPtr, ProgramCache> programCache;
    std::map<mx::MaterialPtr, std::map<std::string, mx::ValuePtr>> defaultValues;

    void setProgram(mx::GlslMaterialPtr material, mx::GlslProgramPtr program)
    {
        std::vector<mx::MeshPartitionPtr> assignedMeshes;
        for (auto& [mesh, materialAssignment] : viewer->_materialAssignments) {
            if (materialAssignment == material) {
                assignedMeshes.push_back(mesh);
            }
        }

        for (auto& mesh: assignedMeshes) {
            viewer->assignMaterial(mesh, material, false);
        }

        material->setProgram(program);
    }

    std::string setShaderFromSource(mx::MaterialPtr _material, std::string vertex, std::string fragment)
    {
        auto material = std::dynamic_pointer_cast<mx::GlslMaterial>(_material);
        if (!material)
        {
            std::cout << "No material selected!" << std::endl;
            return "invalid material state!";
        }

        if (auto cached = this->programCache[material].findProgram(vertex, fragment))
        {
            setProgram(material, cached);
        }

        mx::GlslProgramPtr program = mx::GlslProgram::create();
        program->addStage(mx::Stage::VERTEX, vertex);
        program->addStage(mx::Stage::PIXEL, fragment);

        try {
            program->build();
        } catch (mx::ExceptionRenderError& e) {
            std::cout << "Failed to compile shader: " << e.what() << std::endl;
            std::string full_error;
            for (auto& line : e.errorLog()) {
                std::cout << line << std::endl;
                full_error += line + "\n";
            }

            return full_error;
        }

        setProgram(material, program);
        programCache[material].storeProgramInCache(material, false, vertex, fragment);
        return "";
    }

    void reset(const drogon::HttpRequestPtr& req,
        std::function<void (const drogon::HttpResponsePtr &)> &&callback)
    {
        ng::async([=] () mutable {
            auto value = req->getJsonObject();

            bool resetUniforms = true;
            bool resetShader = true;
            if (value && value->isObject()) {
                resetUniforms = value->get("resetUniforms", true).asBool();
            }
            if (value && value->isObject()) {
                resetShader = value->get("resetShader", true).asBool();
            }

            for (auto& mat : viewer->_materials) {
                auto material = std::dynamic_pointer_cast<mx::GlslMaterial>(mat);
                if (resetShader) {
                    if (auto cached = this->programCache[material].getDefault()) {
                        setProgram(material, cached);
                    } else {
                        material->unbindGeometry();
                        material->generateShader(viewer->getGenContext());
                        material->bindShader();
                        programCache[material].storeProgramInCache(material, true);
                    }
                }

                if (resetUniforms) {
                    for (auto& uniform: this->defaultValues[mat]) {
                        if (material->findUniform(uniform.first)) {
                            material->modifyUniform(uniform.first, uniform.second);
                        }
                    }
                }
            }

            if (resetUniforms) {
                viewer->setCameraPosition(DEFAULT_CAMERA_POSITION);
                viewer->setCameraTarget(mx::Vector3(0, 0, 0));
            }

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

            r["allmaterials"] = Json::Value(Json::arrayValue);
            for (auto& material : viewer->_materials)
            {
                if (auto shader = material->getShader())
                {
                    Json::Value v;
                    v["vertex"] = shader->getSourceCode(mx::Stage::VERTEX);
                    v["fragment"] = shader->getSourceCode(mx::Stage::PIXEL);
                    r["allmaterials"].append(v);
                }
            }

            callback(drogon::HttpResponse::newHttpJsonResponse(r));
        });
    }

    void append_uniform(Json::Value& list, const mx::UIPropertyItem& item)
    {
        auto path = item.variable->getPath();
        auto material = viewer->getSelectedMaterial();
        if (!material->findUniform(path))
        {
            // Potentially optimized out, see Util.cpp
            return;
        }

        const auto& store_default = [&] () {
            if (!this->defaultValues[material].count(path))
            {
                // store in cache for later resets
                this->defaultValues[material][path] = item.variable->getValue();
            }
        };

        auto value = item.variable->getValue();
        auto defaultValue = this->defaultValues[material].count(path) ? this->defaultValues[material][path] : value;

        if (value->getTypeString() == "float")
        {
            // TODO: is this meaningful?
            float min = -10;
            float max = 10;
            if (item.ui.uiMin)
                min = item.ui.uiMin->asA<float>();
            if (item.ui.uiMax)
                max = item.ui.uiMax->asA<float>();

            auto j = vecUniformToJson(item.variable->getPath(), 1, min, max, "float");
            j["value"] = Json::arrayValue;
            j["value"].append(defaultValue->asA<float>());
            list.append(j);
            store_default();
        }
        else if (value->getTypeString() == "color3")
        {
            auto j = vecUniformToJson(item.variable->getPath(), 3, 0, 1, "color3");
            auto v = defaultValue->asA<mx::Color3>();
            j["value"] = Json::arrayValue;
            j["value"].append(v[0]);
            j["value"].append(v[1]);
            j["value"].append(v[2]);
            list.append(j);
            store_default();
        }
        else if (value->getTypeString() == "boolean")
        {
            auto j = boolUniformToJson(item.variable->getPath());
            j["value"] = defaultValue->asA<bool>();
            list.append(j);
            store_default();
        } else if (value->getTypeString() == "string")
        {
            // usually resource paths, we can't really change those...
            // unless we want to generate random textures which doesn't seem very meaningful
            return;
        }
        else
        {
            std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! Unknown type: " << value->getTypeString() << std::endl;
        }
    }

    std::optional<std::string> setValue(mx::MaterialPtr material, Json::Value& uniformValue, mx::ShaderPort *uniform)
    {
        auto path = uniformValue["name"].asString();
        auto value = uniformValue["value"];

        if (!this->defaultValues[material].count(path))
        {
            // store in cache for later resets
            this->defaultValues[material][path] = uniform->getValue();
        }

        mx::ValuePtr newValue;

        if (uniform->getValue()->getTypeString() == "float")
        {
            if (!value.isArray() || value.size() != 1 || !value[0].isDouble())
            {
                return "Invalid float value for " + path;
            }

            newValue = mx::Value::createValue(float(value[0].asDouble()));
        }
        else if (uniform->getValue()->getTypeString() == "color3")
        {
            if (!value.isArray() || value.size() != 3 || !value[0].isDouble() || !value[1].isDouble() || !value[2].isDouble())
            {
                return "Invalid color value for " + path;
            }

            newValue = mx::Value::createValue(mx::Color3(value[0].asDouble(), value[1].asDouble(), value[2].asDouble()));
        }
        else if (uniform->getValue()->getTypeString() == "boolean")
        {
            if (!value.isBool())
            {
                return "Invalid boolean value for " + path;
            }

            newValue = mx::Value::createValue(value.asBool());
        }
        else
        {
            std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! Unknown type for set: " << uniform->getValue()->getTypeString() << std::endl;
        }

        if (newValue)
        {
            material->modifyUniform(path, newValue);
            viewer->updateUniform(path, newValue);
        }

        return {};
    }

    void getuniforms(const drogon::HttpRequestPtr& req,
        std::function<void (const drogon::HttpResponsePtr &)> &&callback)
    {
        ng::async([this, callback] () mutable {
            Json::Value r = Json::arrayValue;

            auto cam = vecUniformToJson("camera", 3, -5, 5, "camera");
            cam["value"] = Json::arrayValue;
            cam["value"].append(viewer->getCameraPosition()[0]);
            cam["value"].append(viewer->getCameraPosition()[1]);
            cam["value"].append(viewer->getCameraPosition()[2]);
            r.append(cam);

            if (this->enableLookAt)
            {
                auto lookat = vecUniformToJson("lookAt", 1, 0, 1, "lookAt");
                lookat["value"] = 0;
                r.append(lookat);
            }

            if (auto material = viewer->getSelectedMaterial())
            {
                if (material->getPublicUniforms() && !this->disableMaterialUniforms)
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

    drogon::HttpResponsePtr set_uniforms_from_json(const Json::Value& req)
    {
        auto material = std::dynamic_pointer_cast<mx::GlslMaterial>(viewer->getSelectedMaterial());
        if (!material)
        {
            return drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest, drogon::CT_NONE);
        }

        for (auto it = req.begin(); it != req.end(); it++) {
            auto uniformValue = *it;
            if (!uniformValue.isObject() ||
                !uniformValue.isMember("name") || !uniformValue["name"].isString() ||
                !uniformValue.isMember("value"))
            {
                auto resp = drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                        drogon::ContentType::CT_TEXT_HTML);
                resp->setBody("Invalid request: wrong syntax (expected array of name and value)");
                return resp;
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
                    return resp;
                }

                std::cout << "Setting camera" << std::endl;

                viewer->setCameraPosition(mx::Vector3(
                        uniformValue["value"][0].asDouble(), uniformValue["value"][1].asDouble(), uniformValue["value"][2].asDouble()));
            }

            if (name == "lookAt") {
                if (!uniformValue["value"].isArray() || uniformValue["value"].size() != 1 || !uniformValue["value"][0].isDouble()) {
                    auto resp = drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                            drogon::ContentType::CT_TEXT_HTML);
                    resp->setBody("Invalid request: wrong syntax (lookAt should be a [float]!)");
                    return resp;
                }

                std::vector<mx::Vector3> dirs = {
                    {0, 0, 0},
                    {1.0, 0, 0},
                    {-1.0, 0, 0},
                    {0, 0, 1.0},
                    {0, 0, -1.0},
                };

                int d = std::min(int(uniformValue["value"][0].asDouble() * dirs.size()), int(dirs.size()) - 1);
                viewer->setCameraTarget(dirs[d]);
            }

            if (auto uniform = material->findUniform(name))
            {
                if (auto err = setValue(material, uniformValue, uniform))
                {
                    auto resp = drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                            drogon::ContentType::CT_TEXT_HTML);
                    resp->setBody(err.value());
                    return resp;
                }
            }
        }

        std::dynamic_pointer_cast<mx::GlslMaterial>(material)->updateTransparency(viewer->getGenContext());
        return drogon::HttpResponse::newHttpResponse();
    }

    void setuniforms(const drogon::HttpRequestPtr& _req,
        std::function<void (const drogon::HttpResponsePtr &)> &&callback)
    {
        std::cout << "setuniforms" << glfwGetTime() << std::endl;
        auto req = _req->getJsonObject();
        if (!req || !req->isArray()) {
            std::cout << "Invalid request: /setuniforms: expected json array" << std::endl;
            callback(drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                    drogon::ContentType::CT_TEXT_HTML));
            return;
        }

        ng::async([this, callback, req] () mutable {
            auto resp = set_uniforms_from_json(*req);
            callback(resp);
            std::cout << "setuniforms done" << glfwGetTime() << std::endl;
        });
    }

    drogon::HttpResponsePtr set_shader_from_json(const Json::Value& req)
    {
        if (req.isArray()) {
            for (auto material : req) {
                auto resp = set_shader_from_json(material);
                if (resp->statusCode() != drogon::k200OK) {
                    return resp;
                }
            }
            return drogon::HttpResponse::newHttpResponse();
        }

        int material_idx = req.get("material-idx", viewer->getSelectedMaterialIndex()).asInt();
        if (auto material = viewer->getMaterial(material_idx))
        {
            auto old_vertex = material->getShader()->getSourceCode(mx::Stage::VERTEX);
            auto old_fragment = material->getShader()->getSourceCode(mx::Stage::PIXEL);

            auto vertex = req.get("vertex", old_vertex).asString();
            auto fragment = req.get("fragment", old_fragment).asString();

            auto error = setShaderFromSource(material, vertex, fragment);
            if (error == "")
            {
                std::cout << "Successfully set shader for material=" << material_idx
                    << " ctime=" << glfwGetTime() << std::endl;
                return drogon::HttpResponse::newHttpResponse();
            } else
            {
                setShaderFromSource(material, old_vertex, old_fragment);
                auto resp = drogon::HttpResponse::newHttpResponse(drogon::k418ImATeapot,
                    drogon::ContentType::CT_TEXT_PLAIN);
                resp->setBody(error);
                return resp;
            }
        } else {
            std::cout << "Missing selected material ?!?" << std::endl;
            auto resp = drogon::HttpResponse::newHttpResponse(drogon::k418ImATeapot,
                drogon::ContentType::CT_TEXT_PLAIN);
            resp->setBody("unknown error");
            return resp;
        }
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
        std::cout << "Got request for set shader " << glfwGetTime() << std::endl;

        ng::async([=] () mutable
        {
            std::cout << "Dispatching request for set shader" << std::endl;
            callback(set_shader_from_json(*req));
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
        //std::cout << "Pending screenshot: " << " width=" << w << " height=" << h << " " << glfwGetTime() << std::endl;

        ng::async([=] () mutable {
            if (req->isMember("variants") &&
                req->get("variants", Json::nullValue).isArray())
            {
                Json::Value response = Json::arrayValue;
                auto variants = req->get("variants", Json::nullValue);
                size_t offset = 0;

                int mapfd = -1;
                void *mapptr = nullptr;
                size_t mapfile_size = w * h * 3 * variants.size();

                if (req->isMember("mapfile") and req->get("mapfile", Json::nullValue).isString()) {
                    std::string mapfile = req->get("mapfile", Json::nullValue).asString();

                    mapfd = shm_open(mapfile.c_str(), O_RDWR, 0);
                    if (mapfd == -1) {
                        std::cout << "Failed to open mapfile: " << mapfile << std::endl;
                        callback(drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                                drogon::ContentType::CT_TEXT_HTML));
                        return;
                    }

                    mapptr = mmap(nullptr, mapfile_size, PROT_WRITE, MAP_SHARED, mapfd, 0);
                    if (mapptr == MAP_FAILED) {
                        std::cout << "Failed to mmap mapfile: " << mapfile << " " << strerror(errno) << std::endl;
                        close(mapfd);
                        callback(drogon::HttpResponse::newHttpResponse(drogon::k400BadRequest,
                                drogon::ContentType::CT_TEXT_HTML));
                        return;
                    }
                }


                for (int i = 0; i < (int)variants.size(); ++i)
                {
                    if (variants[i].isObject())
                    {
                        if (variants[i].isMember("shaders")) {
                            if (set_shader_from_json(variants[i]["shaders"])->statusCode() != drogon::k200OK) {
                                continue;
                            }
                        }

                        if (variants[i].isMember("uniforms")) {
                            if (set_uniforms_from_json(variants[i]["uniforms"])->statusCode() != drogon::k200OK) {
                                continue;
                            }
                        }

                        auto imgdata = viewer->getNextRender(w, h);
                        Json::Value img = Json::objectValue;
                        img["width"] = imgdata->getWidth();
                        img["height"] = imgdata->getHeight();
                        img["offset"] = offset;

                        size_t bytesize = imgdata->getWidth() * imgdata->getHeight() * 3;
                        if (mapptr) {
                            memcpy((char*)mapptr + offset, imgdata->getResourceBuffer(), bytesize);
                        } else {
                            img["data"] = drogon::utils::base64Encode(
                                (const unsigned char*)imgdata->getResourceBuffer(), bytesize);
                        }

                        offset += bytesize;
                        response.append(img);
                    }
                }

                if (mapptr) {
                    msync(mapptr, mapfile_size, MS_SYNC);
                    munmap(mapptr, mapfile_size);
                    close(mapfd);
                }

                callback(drogon::HttpResponse::newHttpJsonResponse(response));
            } else
            {
                // single screenshot, older interface
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
            }
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
            GLuint64 speed = viewer->runBenchmark(warmup, nr_frames, width, height);
            std::cout << "Results: " << speed << std::endl;
            Json::Value r;
            r["speed"] = speed;
            callback(drogon::HttpResponse::newHttpJsonResponse(r));
        });
    }

    ng::ref<Viewer> viewer;
    bool disableMaterialUniforms = false;
    bool enableLookAt = false;
    ServerController(ng::ref<Viewer> viewer, bool disableMaterialUniforms, bool enableLookAt) :
        viewer(viewer), disableMaterialUniforms(disableMaterialUniforms), enableLookAt(enableLookAt) {}
};

/**
 * A simple class which encapsulates the state used for setting the shader and current material remotely.
 */
class Server {
    bool disableMaterialUniforms;
    bool enableLookAt;
  public:
    void start_server(ng::ref<Viewer> viewer, int port, bool disableMaterialUniforms, bool enableLookAt) {
        this->disableMaterialUniforms = disableMaterialUniforms;
        this->enableLookAt = enableLookAt;
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
            .setClientMaxBodySize(std::numeric_limits<size_t>::max())
            .setLogPath("./")
            .setLogLevel(trantor::Logger::kWarn)
            .addListener("0.0.0.0", port)
            .setThreadNum(1)
            .registerController(std::make_shared<ServerController>(viewer, disableMaterialUniforms, enableLookAt))
            .run();
    }

    bool main_running = true;
    std::thread server_thread;
    std::thread reset_sigint_handler;
};
