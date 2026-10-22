// Deterministic vector fixtures: no network, model, or embedding service.
#include "agent_rpc/mcp/rag/vector_index.h"
#include "agent_rpc/mcp/mcp_agent_integration.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace agent_rpc::mcp::rag;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static IndexedTool tool(const std::string& name, float angle) {
    const float radians = angle * 3.14159265358979323846f / 180.0f;
    IndexedTool result;
    result.name = name;
    result.embedding = {std::cos(radians), std::sin(radians)};
    return result;
}

static std::vector<std::string> names(const std::vector<SearchResult>& results) {
    std::vector<std::string> value;
    for (const auto& result : results) value.push_back(result.tool.name);
    return value;
}

int main() {
    try {
        VectorIndex index;
        index.addTool(tool("weather", 20.0f));
        index.addTool(tool("weather_history", 21.0f));
        index.addTool(tool("log", -45.0f));
        const std::vector<float> query = {1.0f, 0.0f};
        MMRConfig mmr;
        mmr.enabled = true;
        mmr.fetch_k = 3;
        const auto ordinary = index.search(query, 2, 0.3f);
        const auto diverse = index.searchMMR(query, 2, mmr, 0.3f);
        require(names(ordinary) == std::vector<std::string>({"weather", "weather_history"}),
                "ordinary ranking fixture is invalid");
        require(names(diverse) == std::vector<std::string>({"weather", "log"}),
                "MMR should retain relevance while reducing redundant candidates");
        require(std::abs(diverse[1].similarity - std::sqrt(0.5f)) < 1e-5f,
                "returned score must remain original cosine, not MMR score");

        mmr.lambda = 1.0f;
        require(names(index.searchMMR(query, 2, mmr, 0.3f)) == names(ordinary),
                "lambda=1 must recover ordinary ranking");
        mmr.lambda = 0.7f;
        mmr.fetch_k = 2;
        require(names(index.searchMMR(query, 2, mmr, 0.3f)) == names(ordinary),
                "MMR must not select outside the fetch pool");
        mmr.fetch_k = 30;
        require(index.searchMMR(query, 10, mmr, 0.3f).size() == 3,
                "small indexes must not produce duplicates or padding");
        const auto filtered = index.searchMMR(query, 2, mmr, 0.8f);
        require(names(filtered) == names(ordinary), "threshold must filter before MMR");
        const auto fallback = index.searchMMR(query, 2, mmr, 1.0f);
        require(fallback.size() == 1 && fallback.front().tool.name == "weather",
                "preserve the existing best-match fallback when all scores are below threshold");

        VectorIndex tied;
        tied.addTool(tool("z", 0.0f));
        tied.addTool(tool("a", 0.0f));
        require(names(tied.searchMMR(query, 2, mmr)) == std::vector<std::string>({"a", "z"}),
                "ties must have stable tool-name ordering");
        require(index.search(query, 0).empty() && index.search(query, -1).empty(),
                "invalid ordinary limits must not resize vectors");
        for (const float value : {-0.1f, 1.1f, std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::infinity()}) {
            mmr.lambda = value;
            bool rejected = false;
            try { index.searchMMR(query, 2, mmr); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected, "invalid MMR weight was accepted");
        }
        mmr.lambda = 0.7f;
        mmr.fetch_k = 1;
        bool rejected = false;
        try { index.searchMMR(query, 2, mmr); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "pool smaller than output limit was accepted");
        mmr.fetch_k = 30;

        VectorIndex invalid;
        for (const auto& embedding : std::vector<std::vector<float>>{
                 {}, {0.0f, 0.0f}, {1.0f},
                 {std::numeric_limits<float>::quiet_NaN(), 0.0f},
                 {std::numeric_limits<float>::infinity(), 0.0f}}) {
            IndexedTool bad;
            bad.name = std::to_string(invalid.size());
            bad.embedding = embedding;
            invalid.addTool(bad);
        }
        require(invalid.searchMMR(query, 2, mmr).empty(), "invalid vectors entered MMR pool");
        invalid.addTool(tool("valid", 0.0f));
        require(names(invalid.searchMMR(query, 2, mmr)) == std::vector<std::string>({"valid"}),
                "valid tool lost among malformed vectors");
        require(invalid.searchMMR({0.0f, 0.0f}, 2, mmr).empty(), "zero query vector accepted");
        require(std::abs(VectorIndex::cosineSimilarity(
                    {std::numeric_limits<float>::max()}, {std::numeric_limits<float>::max()}) - 1.0f) < 1e-6f,
                "cosine overflowed for finite vectors");

        std::vector<std::string> arguments = {"test", "--enable-rag", "--rag-mmr",
                                            "--rag-mmr-fetch-k", "40", "--rag-mmr-lambda", "0.8"};
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(argument.data());
        const auto config = agent_rpc::mcp::parseMCPConfigFromArgs(static_cast<int>(argv.size()), argv.data());
        require(config.rag_config.enable_mmr && config.rag_config.mmr_fetch_k == 40 &&
                    std::abs(config.rag_config.mmr_lambda - 0.8f) < 1e-6f,
                "integration command-line MMR configuration was not parsed");
        std::cout << "[PASS] MMR diversity, budgets, thresholds, validation, ties and integration flags\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
