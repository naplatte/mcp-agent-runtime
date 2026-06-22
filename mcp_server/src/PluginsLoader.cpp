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

#include "PluginsLoader.h"

namespace vx::mcp {

    PluginsLoader::PluginsLoader() = default;

    PluginsLoader::~PluginsLoader() {
        UnloadPlugins();
    }

    bool PluginsLoader::LoadPlugins(const std::string& directory) {
        try {
            // 加载路径中的所有插件
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
                if (entry.is_regular_file()) {
                    std::string extension = entry.path().extension().string();

                    // Check if this is a shared library
                    if (extension == ".so")
                    {
                        LoadPlugin(entry.path().string());
                    }
                }
            }
            return true;
        } catch (const std::exception& ex) {
            LOG(ERROR) << "Error loading plugins: " << ex.what() << std::endl;
            return false;
        }
    }

    bool PluginsLoader::LoadPlugin(const std::string& path) {
        PluginEntry entry;
        entry.path = path;

        // Load the shared library
        entry.handle = dlopen(path.c_str(), RTLD_LAZY);
        if (!entry.handle) {
            LOG(ERROR) << "Failed to load plugin: " << path << " - " << dlerror() << std::endl;
            return false;
        }

        // 获取该插件的两个函数（以函数指针的形式获取） 
        // dlsym:根据给定的句柄 (handle) 和符号名 (symbol)，返回该符号在内存中的地址
        entry.createFunc = (PluginAPI * (*)())dlsym(entry.handle, "CreatePlugin");
        entry.destroyFunc = (void (*)(PluginAPI *))dlsym(entry.handle, "DestroyPlugin");

        // Check if required functions were found
        if (!entry.createFunc || !entry.destroyFunc) {
            LOG(ERROR) << "Plugin does not export required functions: " << path << std::endl;

            dlclose(entry.handle);

            return false;
        }

        // 创建插件实例
        entry.instance = entry.createFunc();

        // 初始化插件
        if (!entry.instance->Initialize()) {
            LOG(ERROR) << "Plugin initialization failed: " << path << std::endl;
            entry.destroyFunc(entry.instance);

            dlclose(entry.handle);

            return false;
        }

        // 将其添加到插件list
        m_plugins.push_back(entry);
        LOG(INFO) << "Loaded plugin: " << entry.instance->GetName()
                  << " v" << entry.instance->GetVersion() << std::endl;

        return true;
    }

    void PluginsLoader::UnloadPlugins() {
        for (auto& entry : m_plugins) {
            UnloadPlugin(entry);
        }
        m_plugins.clear();
    }

    void PluginsLoader::UnloadPlugin(PluginEntry& entry) {
        if (entry.instance) {
            // Shutdown the plugin
            entry.instance->Shutdown();

            // Destroy the plugin instance
            entry.destroyFunc(entry.instance);
            entry.instance = nullptr;
        }

        // Unload the library
        if (entry.handle) {
            dlclose(entry.handle);
            entry.handle = nullptr;
        }
    }

    const std::vector<PluginEntry>& PluginsLoader::GetPlugins() const {
        return m_plugins;
    }

}