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

#ifndef MCP_SERVER_PLUGINAPI_H
#define MCP_SERVER_PLUGINAPI_H

#define PLUGIN_API __attribute__((visibility("default")))

#ifdef __cplusplus
// 使用 C 语言的符号命名规则
extern "C" {
#endif

typedef void (*ClientNotificationCallback)(const char* pluginName, const char* notification);

typedef enum {
    PLUGIN_TYPE_TOOLS = 0,
    PLUGIN_TYPE_PROMPTS = 1,
    PLUGIN_TYPE_RESOURCES = 2
} PluginType;

typedef struct {
    const char* name;
    const char* description;
    const char* inputSchema;  // 字符串形式的 JSON schema
} PluginTool;

typedef struct {
    const char* name;
    const char* description;
    const char* arguments;  // 字符串形式的 JSON 参数
} PluginPrompt;

typedef struct {
    const char* name;
    const char* description;
    const char* uri;
    const char* mime;
} PluginResource;

typedef struct {
    ClientNotificationCallback SendToClient;    // 由 server 注入，插件侧不应修改
} NotificationSystem;

// Optional export: the original PluginAPI layout/ABI is unchanged.
typedef struct {
    void* user_data;
    int (*IsCancelled)(void* user_data);
} PluginRequestContext;
typedef char* (*HandleRequestWithContextFn)(const char* request, const PluginRequestContext* context);

typedef struct {
    const char* (*GetName)();
    const char* (*GetVersion)();
    PluginType (*GetType)();
    int (*Initialize)();
    char* (*HandleRequest)(const char* request);
    void (*Shutdown)();
    int (*GetToolCount)();
    const PluginTool* (*GetTool)(int index);
    int (*GetPromptCount)();
    const PluginPrompt* (*GetPrompt)(int index);
    int (*GetResourceCount)();
    const PluginResource* (*GetResource)(int index);
    NotificationSystem* notifications;
} PluginAPI;

PLUGIN_API PluginAPI* CreatePlugin(); // 返回一个api实例
PLUGIN_API void DestroyPlugin(PluginAPI*);

#ifdef __cplusplus
}
#endif

#endif //MCP_SERVER_PLUGINAPI_H
