#include "agent_rpc/mcp/rag/keyword_index.h"
#include "agent_rpc/mcp/mcp_agent_integration.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace agent_rpc::mcp::rag;

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        const auto tokens = KeywordIndex::tokenize("GitHub_createIssue ALPHA-2042 本地目录");
        for (const auto& expected : {"github_createissue", "github", "create", "issue", "alpha-2042", "目录"})
            require(std::find(tokens.begin(), tokens.end(), expected) != tokens.end(), "identifier/UTF-8 token lost");
        KeywordIndex index;
        index.addTool("notify_device", "Send a notification to a mobile device.");
        index.addTool("dispatch_mail", "Send an email message to a recipient address.");
        index.addTool("walk_folder", "Enumerate a local directory and its files.");
        index.addTool("incident", "Resolve incident ALPHA-2042.");
        require(index.search("把故障信息发送到邮箱", 2).front().name == "dispatch_mail", "Chinese/English aliases failed");
        require(index.search("列出文件夹", 1).front().name == "walk_folder", "directory query failed");
        require(index.search("ALPHA-2042", 1).front().name == "incident", "literal identifier failed");
        require(index.search("不存在的词xyzunknown", 10).empty(), "unmatched query invented candidates");
        require(index.search("", 10).empty() && index.search("email", 0).empty(), "invalid query budget accepted");
        index.addTool("incident", "Export deployment manifests.");
        require(index.search("ALPHA-2042", 10).empty(), "updated tool retained stale postings");
        require(index.removeTool("dispatch_mail") && !index.removeTool("dispatch_mail"), "remove result wrong");
        require(index.search("email", 10).empty(), "removed tool retained postings");
        index.clear();
        require(index.search("directory", 10).empty(), "cleared index retained postings");

        const std::vector<std::string> dense = {"first", "second", "target"};
        require(reciprocalRankFusion(dense, {"target"}, 2).front() == "target", "RRF did not combine route evidence");
        require(reciprocalRankFusion({"first", "second"}, {"target"}, 2) ==
                    std::vector<std::string>({"first", "target"}), "keyword-only candidate could not enter final budget");
        require(reciprocalRankFusion(dense, {}, 3) == dense, "no keyword match must preserve dense ordering");
        require(reciprocalRankFusion({"first", "first", "second"}, {"second", "second"}, 3) ==
                    reciprocalRankFusion({"first", "second"}, {"second"}, 3), "duplicate IDs inflated RRF scores");
        bool rejected = false;
        try { reciprocalRankFusion(dense, {}, 2, -1); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "negative fusion constant accepted");

        std::vector<std::string> args = {"test", "--enable-rag", "--rag-hybrid", "--rag-top-k", "20",
                                       "--rag-hybrid-fetch-k", "40", "--rag-rrf-k", "50"};
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        const auto config = agent_rpc::mcp::parseMCPConfigFromArgs(argv.size(), argv.data());
        require(config.rag_config.enable_hybrid && config.rag_config.top_k == 20 &&
                config.rag_config.hybrid_fetch_k == 40 && config.rag_config.hybrid_rrf_k == 50,
                "hybrid integration flags failed");
        std::cout << "[PASS] BM25 postings, identifiers, Chinese aliases, lifecycle, RRF and integration flags\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
