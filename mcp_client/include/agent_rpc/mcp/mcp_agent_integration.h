// MCP Agent 集成层，简化 AI Agent 对 MCP 客户端能力的使用。

#pragma once

#include "mcp_client.h"
#include "agent_rpc/common/logger.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>

// 前向声明
namespace agent_rpc {
namespace mcp {
namespace rag {
    class ToolRetriever;
    struct RetrieverConfig;
    struct RetrievedTool;
}
}
}

namespace agent_rpc {
namespace mcp {

// RAG-MCP 配置
struct RAGConfig {
    bool enabled = false;                     // 是否启用 RAG-MCP
    std::string api_key;                      // DashScope API Key，可从环境变量读取
    std::string model = "text-embedding-v2"; // Embedding 模型
    int top_k = 5;                            // 返回工具数量
    std::string index_path;                   // 索引文件路径
    bool enable_cache = true;                 // 是否启用缓存
    size_t cache_max_size = 1000;             // 缓存最大条目
    int cache_ttl_seconds = 3600;             // 缓存过期时间
    bool enable_mmr = false;                  // 是否使用 MMR 选择候选
    int mmr_fetch_k = 30;                      // MMR 候选池上限
    float mmr_lambda = 0.7f;                  // MMR 相关性权重
    bool enable_hybrid = false;              // 向量 + 本地关键词倒排
    int hybrid_fetch_k = 50;                  // 每路候选上限
    int hybrid_rrf_k = 60;                    // 融合常数，不是返回工具数量
};

// MCP Agent 集成配置
struct MCPAgentConfig {
    std::string mcp_server_path;           // MCP Server 可执行文件路径
    std::vector<std::string> mcp_args;     // MCP Server 启动参数
    bool enable_mcp = false;               // 是否启用 MCP
    int connection_timeout_ms = 5000;      // 连接超时（毫秒）
    int tool_call_timeout_ms = 30000;      // 工具调用超时（毫秒）
    int max_retry_count = 3;               // 最大重试次数
    int retry_delay_ms = 1000;             // 重试延迟（毫秒）
    
    // RAG-MCP 配置
    RAGConfig rag_config;                  // RAG-MCP 配置
};

// 工具调用结果
struct ToolCallResult {
    bool success = false;                  // 是否成功
    std::string result;                    // 成功时的结果
    std::string error;                     // 失败时的错误信息
    int64_t duration_ms = 0;               // 调用耗时（毫秒）
};

// 工具信息
struct ToolInfo {
    std::string name;                      // 工具名称
    std::string description;               // 工具描述
    std::string input_schema;              // 输入参数 JSON Schema
};

// 封装 MCPClient 和 MCPToolManager，提供面向 AI Agent 的 MCP 接口。
class MCPAgentIntegration {
public:
    MCPAgentIntegration();
    ~MCPAgentIntegration();
    
    // 禁止拷贝
    MCPAgentIntegration(const MCPAgentIntegration&) = delete;
    MCPAgentIntegration& operator=(const MCPAgentIntegration&) = delete;
    
    // 生命周期管理

    // 初始化 MCP 集成；MCP 未启用或连接失败时进入降级模式。
    bool initialize(const MCPAgentConfig& config);
    
    // 断开 MCP Server 并释放资源。
    void shutdown();
    
    // 检查 MCP 集成是否已初始化。
    bool isInitialized() const { return initialized_; }
    
    // 检查 MCP Client 是否已连接并可用。
    bool isAvailable() const;
    
    // 工具管理
    
    // 获取缓存中的可用工具列表。
    std::vector<ToolInfo> getAvailableTools() const;
    
    // 获取所有可用工具的名称。
    std::vector<std::string> getToolNames() const;
    
    // 检查指定工具是否可用。
    bool hasToolAvailable(const std::string& tool_name) const;
    
    // 获取指定工具的描述，不存在时返回空字符串。
    std::string getToolDescription(const std::string& tool_name) const;
    
    // 获取指定工具的输入 Schema，不存在时返回空字符串。
    std::string getToolInputSchema(const std::string& tool_name) const;
    
    // 工具调用

    // 同步调用 MCP 工具并返回结构化结果。
    ToolCallResult callTool(const std::string& tool_name, 
                            const std::string& arguments);
    
    // 异步调用 MCP 工具，并在完成后执行回调。
    void callToolAsync(const std::string& tool_name,
                       const std::string& arguments,
                       std::function<void(const ToolCallResult&)> callback);
    
    // 简化调用结果，失败时返回带 [ERROR] 前缀的字符串。
    std::string callToolSimple(const std::string& tool_name,
                               const std::string& arguments);
    
    // 配置和状态
    
    // 获取当前 MCP Agent 配置。
    const MCPAgentConfig& getConfig() const { return config_; }
    
    // 获取 MCP Server 可执行文件路径。
    const std::string& getMCPServerPath() const { return config_.mcp_server_path; }
    
    // 获取当前连接状态的文字描述。
    std::string getStatusDescription() const;
    
    // 从 MCP Server 重新获取并缓存工具列表。
    bool refreshTools();
    
    // RAG-MCP 功能
    
    // 检查 RAG-MCP 是否已初始化并可用。
    bool isRAGEnabled() const;
    
    // 检索与查询相关的工具，RAG 未启用时返回全部工具。
    std::vector<ToolInfo> getRelevantTools(const std::string& query) const;
    
    // 按指定 top_k 检索与查询相关的工具。
    std::vector<ToolInfo> getRelevantTools(const std::string& query, int top_k) const;
    
    // 将工具列表转换为 LLM 函数调用格式的 JSON。
    static std::string toFunctionCallingFormat(const std::vector<ToolInfo>& tools);
    
    // 检索相关工具并转换为 LLM 函数调用格式的 JSON。
    std::string getRelevantToolsAsJson(const std::string& query) const;

private:
    // 内部方法
    bool connectToMCPServer();
    void disconnectFromMCPServer();
    void updateToolCache();
    bool initializeRAG();
    void shutdownRAG();
    
    // 成员变量
    MCPAgentConfig config_;
    std::shared_ptr<MCPClient> mcp_client_;
    std::shared_ptr<MCPToolManager> tool_manager_;
    
    // 工具缓存
    std::vector<ToolInfo> tool_cache_;
    mutable std::mutex tool_cache_mutex_;
    
    // RAG-MCP
    std::unique_ptr<rag::ToolRetriever> tool_retriever_;
    
    // 状态
    std::atomic<bool> initialized_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> rag_initialized_{false};
};

// 从命令行参数解析 MCP Agent 配置。
MCPAgentConfig parseMCPConfigFromArgs(int argc, char* argv[]);

// 从环境变量解析 MCP Agent 配置。
MCPAgentConfig parseMCPConfigFromEnv();

} // namespace mcp
} // namespace agent_rpc
