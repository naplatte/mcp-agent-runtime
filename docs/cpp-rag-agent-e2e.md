# C++ RAG + MCP Tool 在线端到端测试

此程序测试 **stdio + Tool**：`MCPAgentIntegration` 连接服务端并发现工具，项目自身的 C++ `ToolRetriever` 使用 DashScope Embedding 筛选工具，再把筛选结果交给 DashScope Chat Completions 模型。模型返回工具调用后，程序调用 `MCPAgentIntegration::callTool`，经过 `MCPToolManager`、`MCPClient`、stdio 服务端和 calculator 插件；随后将插件文本结果回传模型，检查最终回答。

Embedding 和对话模型是外部服务；MCP 通信、插件分发和 RAG 检索使用本项目的 C++ 实现。此场景不验证 Streamable HTTP、Prompt 或 Resource。

## 在 Linux/WSL 构建

需要 C++20 编译器、CMake、pkg-config、libcurl、jsoncpp 和 nlohmann-json 开发包。

```bash
cmake -S . -B build -DBUILD_MCP_EXAMPLES=OFF
cmake --build build --target mcp_server calculator test_rag_agent_e2e -j4
```

把 `DASHSCOPE_API_KEY` 配置为运行环境变量后运行（不要作为命令行参数传入）：

```bash
./build/tests/cpp/test_rag_agent_e2e \
  ./build/mcp_server/mcp_server \
  ./build/mcp_server/plugins/calculator
```

可选第三个参数指定对话模型，默认 `qwen-plus`。插件目录只需包含编译出的 `libcalculator.so`；这个插件提供多个数学工具，足以验证 RAG 的 `top_k=3` 筛选确实比全量工具少。程序检查 calculator/multiply 的发现、RAG 初始化、非空真子集、模型提出的工具调用属于所筛工具、插件结果含 `56088`，以及最终回答含 `56088`。任一环节不满足都返回非零。缺少密钥时返回 77 并打印 `[SKIP]`，不能作为通过记录。

原有 `test_mcp_integration --ai` 仍保留原用法；它是直接客户端的交互演示，不代表上述 RAG 链路。
