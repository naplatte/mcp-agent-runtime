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
//   included in all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
//  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

#ifndef MCP_SERVER_PLUGINS_LOADER_H
#define MCP_SERVER_PLUGINS_LOADER_H

#include <dlfcn.h>
typedef void* LibraryHandle;
#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <filesystem>
#include <algorithm>

#include "aixlog.hpp"
#include "PluginAPI.h"

namespace vx::mcp {

    // 一个已加载插件的信息
    struct PluginEntry {
        std::string path;
        LibraryHandle handle; // 句柄 (类似文件句柄，os给已打开动态库的“凭证”)
        PluginAPI* instance; // 插件实例

        // 函数指针
        PluginAPI* (*createFunc)();
        void (*destroyFunc)(PluginAPI*);
    };

    class PluginsLoader {
    public:
        PluginsLoader();
        ~PluginsLoader();

        // 从目录中加载插件
        bool LoadPlugins(const std::string& directory);

        // 卸载所有插件
        void UnloadPlugins();

        // 获取已加载的插件
        const std::vector<PluginEntry>& GetPlugins() const;

    private:
        bool LoadPlugin(const std::string& path);
        void UnloadPlugin(PluginEntry& entry);

    private:
        std::vector<PluginEntry> m_plugins;
    };

}

#endif //MCP_SERVER_PLUGINS_LOADER_H
