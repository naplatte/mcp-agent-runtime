//  The MIT License
//
//  Copyright (C) 2025 Giuseppe Mastrangelo
//
//  Permission is hereby granted, free of charge, to any person obtaining
//  a copy of this software and associated documentation files (the
//  'Software'), to deal in the Software without restriction, including
//  without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to
//  permit persons to whom the Software is furnished to do so, subject to
//  the following conditions:
//
//  The above copyright notice and this permission notice shall be
//  included in all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
//  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

#include <thread>
#include <algorithm>
#include <cstring>
#include "PluginAPI.h"
#include "json.hpp"

using json = nlohmann::json;

static PluginTool methods[] = {
        {
            "sleep",
            "Pauses execution for the specified number of milliseconds.",
        R"({
            "$schema": "http://json-schema.org/draft-07/schema#",
            "type": "object",
            "properties": {
                "milliseconds": { "type": "number", "minimum": 0, "description": "Number of milliseconds to sleep." }
            },
            "required": ["milliseconds"],
            "additionalProperties": false
        })"
        }
};

const char* GetNameImpl() { return "sleep-tools"; }
const char* GetVersionImpl() { return "1.0.0"; }
PluginType GetTypeImpl() { return PLUGIN_TYPE_TOOLS; }

int InitializeImpl() {
    return 1;
}

char* HandleRequestWithContextImpl(const char* req, const PluginRequestContext* context) {
    auto request = json::parse(req);

    auto milliseconds = request["params"]["arguments"]["milliseconds"].get<int>();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    bool cancelled = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (context && context->IsCancelled && context->IsCancelled(context->user_data)) {
            cancelled = true;
            break;
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::min(remaining,
            std::chrono::steady_clock::duration(std::chrono::milliseconds(10))));
    }

    nlohmann::json responseContent;
    responseContent["type"] = "text";
    responseContent["text"] = cancelled ? "Request cancelled" : "Waited for " + std::to_string(milliseconds) + " milliseconds";

    nlohmann::json response;
    response["content"] = json::array();
    response["content"].push_back(responseContent);
    response["isError"] = cancelled;

    std::string result = response.dump();
    char* buffer = new char[result.length() + 1];
    strcpy(buffer, result.c_str());

    return buffer;
}

char* HandleRequestImpl(const char* req) {
    return HandleRequestWithContextImpl(req, nullptr);
}

extern "C" PLUGIN_API char* HandleRequestWithContext(const char* req, const PluginRequestContext* context) {
    return HandleRequestWithContextImpl(req, context);
}

void ShutdownImpl() {
}

int GetToolCountImpl() {
    return sizeof(methods) / sizeof(methods[0]);
}

const PluginTool* GetToolImpl(int index) {
    if (index < 0 || index >= GetToolCountImpl()) return nullptr;
    return &methods[index];
}

static PluginAPI plugin = {
        GetNameImpl,
        GetVersionImpl,
        GetTypeImpl,
        InitializeImpl,
        HandleRequestImpl,
        ShutdownImpl,
        GetToolCountImpl,
        GetToolImpl,
        nullptr,
        nullptr,
        nullptr,
        nullptr
};

extern "C" PLUGIN_API PluginAPI* CreatePlugin() {
    return &plugin;
}

extern "C" PLUGIN_API void DestroyPlugin(PluginAPI*) {
    // 该示例无需清理资源
}
