#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent_rpc::mcp::rag {

struct KeywordMatch {
    std::string name;
    double score;
};

struct HybridConfig {
    bool enabled = false;
    int fetch_k = 50;   // 每路候选上限，与最终 top_k 分开。
    int rrf_k = 60;     // 排名融合常数，不是返回工具数量。
};

// 内存倒排索引：词 -> 工具及词频，使用 BM25 排序，无外部服务。
class KeywordIndex {
public:
    void addTool(const std::string& name, const std::string& text);
    bool removeTool(const std::string& name);
    void clear();
    std::vector<KeywordMatch> search(const std::string& query, int top_k) const;
    static std::vector<std::string> tokenize(const std::string& text);

private:
    bool removeUnlocked(const std::string& name);
    struct Document {
        std::unordered_map<std::string, size_t> terms;
        size_t length = 0;
    };
    std::unordered_map<std::string, Document> documents_;
    std::unordered_map<std::string, std::unordered_map<std::string, size_t>> postings_;
    size_t total_length_ = 0;
    mutable std::mutex mutex_;
};

// 每个工具在每路只计一次；同分优先向量排名，再按工具名稳定排序。
std::vector<std::string> reciprocalRankFusion(
    const std::vector<std::string>& dense,
    const std::vector<std::string>& keywords, int top_k, int rank_constant = 60);

} // namespace agent_rpc::mcp::rag
