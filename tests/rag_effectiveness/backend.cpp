// JSON-lines bridge to the project's real MCP discovery and C++ RAG modules.
// No retrieval/model-selection algorithm is reimplemented here.
#include "agent_rpc/mcp/mcp_agent_integration.h"
#include "agent_rpc/mcp/rag/tool_retriever.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace agent_rpc::mcp;
using Clock = std::chrono::steady_clock;

static double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int main() {
    // Project logging writes to cout. Keep the protocol on the original stream.
    std::ostream protocol(std::cout.rdbuf());
    std::cout.rdbuf(std::cerr.rdbuf());
    std::unique_ptr<MCPAgentIntegration> integration;
    std::unique_ptr<rag::ToolRetriever> retriever;
    std::set<std::string> indexed;
    std::string index_path;
    int dimension = 1536;
    for (std::string line; std::getline(std::cin, line);) {
        try {
            auto input = json::parse(line);
            const auto op = input.at("op").get<std::string>();
            json output;
            if (op == "discover") {
                MCPAgentConfig config;
                config.enable_mcp = true;
                config.mcp_server_path = input.at("server");
                config.mcp_args = {"-p", input.at("plugins").get<std::string>(),
                                   "-l", input.at("logs").get<std::string>()};
                config.max_retry_count = 0;
                config.rag_config.enabled = false;
                integration = std::make_unique<MCPAgentIntegration>();
                if (!integration->initialize(config) || !integration->isAvailable())
                    throw std::runtime_error("MCP discovery failed");
                const auto tools = integration->getAvailableTools();
                if (tools.empty()) throw std::runtime_error("MCP returned no tools");
                output["tools"] = json::parse(MCPAgentIntegration::toFunctionCallingFormat(tools));
            } else if (op == "index") {
                if (!retriever) {
                    rag::RetrieverConfig config;
                    config.embedding_config.loadApiKeyFromEnv();
                    if (input.contains("fixture_key"))
                        config.embedding_config.api_key = input.at("fixture_key");
                    if (config.embedding_config.api_key.empty())
                        throw std::runtime_error("DASHSCOPE_API_KEY is missing");
                    config.embedding_config.model = input.value("embedding_model", "text-embedding-v2");
                    config.embedding_config.dimension = input.value("dimension", 1536);
                    dimension = config.embedding_config.dimension;
                    config.embedding_config.max_retries = 1;
                    if (input.contains("embedding_url")) config.embedding_config.api_url = input.at("embedding_url");
                    config.similarity_threshold = input.value("threshold", 0.3f);
                    index_path = input.at("index_path");
                    config.index_path = index_path;
                    config.cache_config.enabled = true;
                    config.cache_config.max_size = 2048;
                    retriever = std::make_unique<rag::ToolRetriever>(config);
                    if (!retriever->initialize() || retriever->getIndexSize() != 0)
                        throw std::runtime_error("RAG must start from an empty, fresh index");
                }
                auto start = Clock::now();
                auto before = retriever->getCacheStats();
                std::set<std::string> requested;
                for (const auto& tool : input.at("tools")) {
                    auto name = tool.at("name").get<std::string>();
                    if (!requested.insert(name).second) throw std::runtime_error("Duplicate tool name");
                    if (!indexed.count(name)) {
                        ToolInfo info;
                        info.name = name;
                        info.description = tool.at("description");
                        info.input_schema = tool.at("parameters").dump();
                        retriever->addTool(info);
                    }
                }
                for (const auto& name : indexed)
                    if (!requested.count(name)) throw std::runtime_error("Catalog sizes must grow monotonically");
                if (retriever->getIndexSize() != requested.size() || !retriever->saveIndex())
                    throw std::runtime_error("Incomplete RAG index; refusing effectiveness evaluation");
                // addTool catches errors internally; also reject empty/invalid API vectors.
                std::ifstream file(index_path);
                const auto saved = json::parse(file);
                for (const auto& tool : saved.at("tools")) {
                    const auto vec = tool.at("embedding").get<std::vector<float>>();
                    double norm = 0;
                    for (auto value : vec) {
                        if (!std::isfinite(value)) throw std::runtime_error("Non-finite embedding");
                        norm += value * value;
                    }
                    if (static_cast<int>(vec.size()) != dimension || norm == 0)
                        throw std::runtime_error("Invalid embedding dimension or zero vector");
                }
                indexed = requested;
                auto after = retriever->getCacheStats();
                output = {{"indexed", indexed.size()}, {"build_ms", elapsed(start)},
                          {"cache_misses", after.misses - before.misses}};
            } else if (op == "reset_query_cache") {
                if (!retriever) throw std::runtime_error("Index is not initialized");
                const auto config = retriever->getConfig();
                retriever.reset(); // Persists tool vectors, discards the in-memory query cache.
                retriever = std::make_unique<rag::ToolRetriever>(config);
                if (!retriever->initialize() || retriever->getIndexSize() != indexed.size())
                    throw std::runtime_error("Failed to reload tool vectors for cold-query timing");
                output = {{"indexed", indexed.size()}};
            } else if (op == "retrieve") {
                if (!retriever) throw std::runtime_error("Index is not initialized");
                const int k = input.at("k");
                if (k <= 0 || k >= static_cast<int>(indexed.size()))
                    throw std::runtime_error("k must be positive and smaller than the catalog");
                auto before = retriever->getCacheStats();
                auto start = Clock::now();
                auto selected = retriever->retrieve(input.at("query"), k);
                const double duration = elapsed(start);
                auto after = retriever->getCacheStats();
                if (selected.empty() || selected.size() > static_cast<size_t>(k))
                    throw std::runtime_error("RAG returned no valid subset; no fallback to all tools");
                std::vector<ToolInfo> definitions;
                json scores = json::array();
                for (const auto& tool : selected) {
                    if (!indexed.count(tool.name)) throw std::runtime_error("Unknown retrieved tool");
                    definitions.push_back({tool.name, tool.description, tool.input_schema});
                    scores.push_back({{"name", tool.name}, {"score", tool.relevance_score}});
                }
                output = {{"tools", json::parse(MCPAgentIntegration::toFunctionCallingFormat(definitions))},
                          {"scores", scores}, {"retrieval_ms", duration},
                          {"cache_hits", after.hits - before.hits},
                          {"cache_misses", after.misses - before.misses}};
            } else if (op == "quit") {
                protocol << json({{"ok", true}}).dump() << std::endl;
                break;
            } else {
                throw std::runtime_error("Unknown operation");
            }
            output["ok"] = true;
            protocol << output.dump() << std::endl;
        } catch (const std::exception& error) {
            // Never serialize config, API headers, or the input request in errors.
            protocol << json({{"ok", false}, {"error", error.what()}}).dump() << std::endl;
        }
    }
    if (integration) integration->shutdown();
    return 0;
}
