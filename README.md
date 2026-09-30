# MCP (Model Context Protocol) 完整实现

MCP 协议的完整 C++ 实现，包含 MCP 客户端、MCP 服务器和 RAG-MCP 智能工具检索模块。

从 [agent-communication](../) 项目中独立提取的子项目。

## 使用doxyfile构建：
```
doxygen Doxyfile
cd docs/doxygen_output/html
python3 -m http.server 8080
```
## 项目推送
```
# 1. 看看有没有改动
git status

# 2. 如果有改动，添加并提交
git add .
git commit -m "你的提交信息"

# 3. 推送到 GitHub
git push

# 下拉
## 1. 先看当前状态，确认没有未提交的改动
git status

## 2. 如果有未提交改动，先 stash 或 commit
git stash        # 暂存
# 或
git add . && git commit -m "xxx"

## 3. 拉取最新代码（推荐用 rebase 保持历史干净）
git pull --rebase

## 4. 如果之前 stash 了，恢复
git stash pop

pull异常：
sudo chown -R admin:admin ~/mcp-agent-runtime/.git
git pull
```

## 项目结构

```
mcp-agent-runtime/
├── CMakeLists.txt              # 根构建文件
├── README.md
├── common/                     # 公共组件（日志）
│   ├── include/agent_rpc/common/
│   │   └── logger.h            # 日志系统接口
│   └── src/
│       └── logger.cpp          # 日志实现
├── mcp_client/                 # MCP 客户端库
│   ├── include/agent_rpc/mcp/
│   │   ├── mcp_client.h        # MCP 客户端接口（STDIO + Streamable HTTP）
│   │   ├── mcp_agent_integration.h  # AI Agent 集成辅助类
│   │   └── rag/                # RAG-MCP 模块
│   │       ├── embedding_service.h  # DashScope 向量化服务
│   │       ├── embedding_cache.h    # LRU 向量缓存
│   │       ├── vector_index.h       # 向量索引
│   │       ├── tool_retriever.h     # 工具检索器
│   │       └── tool_validator.h     # 工具验证器
│   └── src/
│       ├── mcp_client.cpp           # 客户端实现
│       ├── mcp_tool_manager.cpp     # 工具管理器
│       ├── mcp_agent_integration.cpp # Agent 集成
│       └── rag/                     # RAG 实现
├── mcp_server/                 # MCP 服务器（独立可执行程序）
│   ├── include/                # 所有服务器头文件
│   │   ├── ITransport.h        # 传输层接口
│   │   ├── PluginAPI.h         # 插件 C ABI 定义
│   │   ├── Server.h            # 核心 JSON-RPC 服务器
│   │   ├── StdioTransport.h    # STDIO 传输实现
│   │   ├── StreamableHttpTransport.h # Streamable HTTP 传输实现
│   │   ├── PluginsLoader.h     # 动态插件加载器
│   │   ├── MCPBuilder.h        # JSON-RPC 响应构建器
│   │   └── SessionBuilder.h    # 会话 ID 生成器
│   ├── src/                    # 实现文件
│   │   ├── main.cpp            # 服务器入口
│   │   ├── Server.cpp          # 核心服务器实现
│   │   ├── StdioTransport.cpp  # STDIO 传输实现
│   │   ├── StreamableHttpTransport.cpp # Streamable HTTP 传输实现
│   │   └── PluginsLoader.cpp   # 插件加载器实现
│   └── plugins/                # 内置插件
│       ├── calculator/         # 计算器
│       ├── weather/            # 天气查询
│       ├── sleep/              # 延时工具
│       ├── code-review/        # 代码审查
│       ├── notification/       # 通知
│       └── bacio-quote/        # 名言
├── third_party/                # 第三方库
│   ├── httplib.h               # cpp-httplib (HTTP 服务器)
│   ├── json.hpp                # nlohmann/json
│   ├── popl.hpp                # 命令行参数解析
│   ├── base64.hpp              # Base64 编解码
│   └── aixlog.hpp              # 日志库
├── tests/                      # 测试脚本与配置
│   ├── test_all.sh             # 统一测试入口
│   ├── cpp/                    # C++ 集成测试 (使用项目自身 mcp_client)
│   │   └── test_mcp_integration.cpp
│   ├── python/                 # Python 测试客户端
│   │   ├── test_ai_mcp.py      # AI 集成测试
│   │   ├── test-client-stdio.py
│   │   ├── test-client-streamable-http.py
│   │   └── test-client-http.py
│   └── config/                 # 测试配置
│       ├── configuration-*.json
│       └── requirements.txt
├── examples/                   # 示例代码
│   └── rag_mcp_example.cpp
└── docs/                       # 文档
    ├── mcp-plugin-development.md
    └── rag-mcp-guide.md
```

## 核心组件

### 1. MCP 客户端 (`mcp_client`)

支持两种传输模式的 MCP 客户端：

- **STDIO 模式**: 通过标准输入输出与本地 MCP Server 进程通信
- **Streamable HTTP 模式**: 通过统一 `/mcp` HTTP endpoint 与远程 MCP Server 通信

主要类：
- `MCPClient` - MCP 客户端（JSON-RPC 协议）
- `MCPToolManager` - 工具管理器
- `MCPServiceIntegrator` - 服务集成器
- `MCPAgentIntegration` - AI Agent 简化集成接口

### 2. RAG-MCP 模块

基于检索增强生成的智能工具选择：

- `EmbeddingService` - 阿里百炼 DashScope 文本向量化
- `EmbeddingCache` - LRU 缓存，减少 API 调用
- `VectorIndex` - 向量索引，余弦相似度搜索
- `ToolRetriever` - 工具检索器，整合以上组件
- `ToolValidator` - 可选的工具验证

### 3. MCP 服务器 (`mcp_server`)

完整的 MCP 服务器实现（MIT License, by Giuseppe Mastrangelo）：

- 插件化架构，支持动态加载 `.so` 插件
- 两种传输模式：STDIO / Streamable HTTP
- 内置 6 个示例插件
- 跨平台支持（Linux / macOS / Windows）

## 依赖项

| 依赖 | 用途 | 模块 |
|------|------|------|
| C++20 编译器 | 编译 | 全部 |
| CMake >= 3.20 | 构建 | 全部 |
| libcurl | HTTP 客户端 | mcp_client |
| jsoncpp | JSON 解析 | mcp_client |
| nlohmann/json | JSON（header-only, 已内置） | mcp_server, mcp_client(RAG) |
| Threads | 多线程 | 全部 |

## 构建

```bash
mkdir build && cd build && cmake ..
make -j1
```

### 仅构建 MCP 服务器

```bash
cd mcp_server
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 构建选项

```bash
# 不构建示例
cmake -DBUILD_MCP_EXAMPLES=OFF ..

# Debug 模式
cmake -DCMAKE_BUILD_TYPE=Debug ..
```

## 快速开始

### 启动 MCP 服务器

```bash
# STDIO 模式（默认）
./build/mcp_server/mcp_server -p ./build/mcp_server/plugins

# Streamable HTTP 模式
./build/mcp_server/mcp_server --streamable-http -p ./build/mcp_server/plugins
```

### 使用 MCP 客户端

```cpp
#include "agent_rpc/mcp/mcp_client.h"

using namespace agent_rpc::mcp;

// STDIO 模式连接
MCPClient client;
client.connect("/path/to/mcp_server", {"--plugins", "./plugins"});

// 列出工具
auto tools = client.listTools();
for (const auto& tool : tools) {
    std::cout << tool.name << ": " << tool.description << std::endl;
}

// 调用工具
auto response = client.callTool("calculator", R"({"expression": "2+3"})");
std::cout << response.result << std::endl;

client.disconnect();
```

### 使用 RAG-MCP 智能工具检索

```cpp
#include "agent_rpc/mcp/mcp_agent_integration.h"

using namespace agent_rpc::mcp;

MCPAgentConfig config;
config.enable_mcp = true;
config.mcp_server_path = "/path/to/mcp_server";
config.rag_config.enabled = true;
config.rag_config.api_key = std::getenv("DASHSCOPE_API_KEY");

MCPAgentIntegration mcp;
mcp.initialize(config);

// 智能检索最相关的工具
auto tools = mcp.getRelevantTools("计算 123 + 456");

// 获取 LLM 函数调用格式
std::string json = mcp.getRelevantToolsAsJson("查询天气");
```

### 测试 MCP 服务器

统一入口脚本，支持三种测试模式：

```bash
bash ./tests/test_all.sh
```

运行后选择模式：

- **模式 1 — AI 集成测试（推荐）**：使用阿里百炼 AI 模型，测试完整 Agent 工具调用链路
- **模式 2 — 基础功能测试**：无需 API Key，用 bash 验证 MCP Server 协议和插件
- **模式 3 — C++ 集成测试**：使用项目自身的 mcp_client 测试 server + plugins（全链路 C++）

#### AI 集成测试

使用阿里百炼 (DashScope) 大模型作为 AI Agent，通过 MCP 协议调用工具，验证完整链路：

```
用户查询 → Embedding 向量化 → RAG 相似度检索 → 筛选相关工具 → LLM (工具选择) → MCP Server (工具执行) → LLM (生成回答)
```

**前置条件**：

- 阿里百炼 API Key（[申请地址](https://dashscope.console.aliyun.com/)）
- Python 3.6+（无需额外依赖）

```bash
# 交互式运行，终端输入 API Key
bash ./tests/test_all.sh
# 选择 1

# 或通过环境变量
DASHSCOPE_API_KEY="sk-xxx" python3 tests/python/test_ai_mcp.py

# 指定模型
DASHSCOPE_API_KEY="sk-xxx" DASHSCOPE_MODEL="qwen-turbo" python3 tests/python/test_ai_mcp.py
```

测试覆盖项：

| 步骤 | 测试内容 |
|------|----------|
| MCP 初始化 | 启动服务器，建立 STDIO 连接 |
| 工具发现 | 获取所有插件注册的工具列表 |
| RAG 索引构建 | 批量 Embedding 向量化所有工具，建立向量索引 |
| RAG + AI 交互式调用 | 用户查询 → 向量相似度筛选工具 → 仅相关工具发给 LLM → MCP 执行 → 回传结果 |
| 基础工具验证 | 直接调用 sleep、calculator、错误处理 |

#### 基础功能测试

无需 API Key，直接验证 MCP Server 协议正确性：

```bash
./tests/test_all.sh
# 选择 2
```

覆盖：initialize、ping、tools/list、calculator、sleep、error handling。

#### 手动测试

```bash
# 通过管道发送 JSON-RPC 请求
echo '{"jsonrpc":"2.0","method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}},"id":"1"}' \
  | ./build/mcp_server/mcp_server -p ./build/mcp_server/plugins -l ./build/test_logs
```

#### C++ 集成测试

使用项目自身的 C++ `mcp_client` 模块连接 `mcp_server`，测试完整的 C++ 链路：

```bash
./tests/test_all.sh
# 选择 3

# 或直接运行
./build/tests/cpp/test_mcp_integration ./build/mcp_server/mcp_server ./build/mcp_server/plugins
```

覆盖：连接 (STDIO)、list_tools、call_tool (weather/sleep)、错误处理、list_prompts、list_resources。

#### Python MCP 客户端测试

```bash
cd tests
pip install -r config/requirements.txt
python3 python/test-client-stdio.py config/configuration-stdio-linux.json
```

## 开发自定义插件

参见 [docs/mcp-plugin-development.md](docs/mcp-plugin-development.md)。

## RAG-MCP 详细指南

参见 [docs/rag-mcp-guide.md](docs/rag-mcp-guide.md)。

## 协议版本

- MCP Client: JSON-RPC 2.0
- MCP Server: v0.7.0

## License

MCP Server 部分基于 MIT License (Copyright (C) 2025 Giuseppe Mastrangelo)。
其余部分遵循原项目许可证。
