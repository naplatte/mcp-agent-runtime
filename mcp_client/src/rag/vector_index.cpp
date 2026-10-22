/**
 * @file vector_index.cpp
 * @brief VectorIndex 实现
 * 
 * Requirements: 1.3, 1.4, 3.1, 3.2, 3.3, 3.4
 * Task 5: 实现 VectorIndex
 */

#include "agent_rpc/mcp/rag/vector_index.h"
#include "agent_rpc/common/logger.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <stdexcept>

using json = nlohmann::json;

namespace agent_rpc {
namespace mcp {
namespace rag {

namespace {
bool validEmbedding(const std::vector<float>& embedding, size_t dimension) {
    if (embedding.empty() || embedding.size() != dimension) return false;
    bool nonzero = false;
    for (float value : embedding) {
        if (!std::isfinite(value)) return false;
        nonzero = nonzero || value != 0.0f;
    }
    return nonzero;
}
} // namespace

void VectorIndex::addTool(const IndexedTool& tool) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    IndexedTool new_tool = tool;
    auto now = std::chrono::system_clock::now().time_since_epoch();
    new_tool.created_at = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    new_tool.updated_at = new_tool.created_at;
    
    tools_[tool.name] = new_tool;
}

bool VectorIndex::removeTool(const std::string& tool_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.erase(tool_name) > 0;
}

bool VectorIndex::updateTool(const IndexedTool& tool) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = tools_.find(tool.name);
    if (it == tools_.end()) {
        return false;
    }
    
    IndexedTool updated_tool = tool;
    updated_tool.created_at = it->second.created_at;
    auto now = std::chrono::system_clock::now().time_since_epoch();
    updated_tool.updated_at = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    
    it->second = updated_tool;
    return true;
}

const IndexedTool* VectorIndex::getTool(const std::string& tool_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = tools_.find(tool_name);
    if (it == tools_.end()) {
        return nullptr;
    }
    return &it->second;
}

bool VectorIndex::hasTool(const std::string& tool_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.find(tool_name) != tools_.end();
}

std::vector<IndexedTool> VectorIndex::getAllTools() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<IndexedTool> result;
    result.reserve(tools_.size());
    for (const auto& pair : tools_) {
        result.push_back(pair.second);
    }
    return result;
}

std::vector<SearchResult> VectorIndex::search(
    const std::vector<float>& query_embedding,
    int top_k,
    float threshold) const {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (top_k <= 0 || tools_.empty() ||
        !validEmbedding(query_embedding, query_embedding.size())) {
        return {};
    }
    
    // 计算所有工具的相似度
    std::vector<SearchResult> all_results;
    all_results.reserve(tools_.size());
    
    for (const auto& pair : tools_) {
        const IndexedTool& tool = pair.second;
        if (!validEmbedding(tool.embedding, query_embedding.size())) {
            continue;
        }
        
        float similarity = cosineSimilarity(query_embedding, tool.embedding);
        
        SearchResult result;
        result.tool = tool;
        result.similarity = similarity;
        all_results.push_back(result);
    }
    
    // 按相似度降序排序
    std::sort(all_results.begin(), all_results.end(),
        [](const SearchResult& a, const SearchResult& b) {
            if (a.similarity != b.similarity) return a.similarity > b.similarity;
            return a.tool.name < b.tool.name;
        });
    
    // 应用阈值过滤
    std::vector<SearchResult> filtered_results;
    for (const auto& result : all_results) {
        if (result.similarity >= threshold) {
            filtered_results.push_back(result);
        }
    }
    
    // 如果没有结果满足阈值，返回最佳匹配
    if (filtered_results.empty() && !all_results.empty()) {
        filtered_results.push_back(all_results[0]);
    }
    
    // 限制返回数量
    if (static_cast<int>(filtered_results.size()) > top_k) {
        filtered_results.resize(top_k);
    }
    
    return filtered_results;
}

std::vector<SearchResult> VectorIndex::searchMMR(
    const std::vector<float>& query_embedding,
    int top_k,
    const MMRConfig& config,
    float threshold) const {
    if (top_k <= 0 || config.fetch_k < top_k)
        throw std::invalid_argument("MMR requires positive top_k and fetch_k >= top_k");
    if (!std::isfinite(config.lambda) || config.lambda < 0.0f || config.lambda > 1.0f)
        throw std::invalid_argument("MMR lambda must be finite and within [0, 1]");

    // search 返回快照，后续两两比较无需长时间持有索引锁。
    auto candidates = search(query_embedding, config.fetch_k, threshold);
    const size_t limit = std::min(static_cast<size_t>(top_k), candidates.size());
    if (candidates.empty() || config.lambda == 1.0f) {
        candidates.resize(limit);
        return candidates;
    }

    std::vector<SearchResult> result;
    result.reserve(limit);
    std::vector<bool> selected(candidates.size(), false);
    std::vector<float> redundancy(candidates.size(), -1.0f);
    size_t best = 0; // 第一个工具始终选择与原问题最相关的候选。
    while (result.size() < limit) {
        selected[best] = true;
        result.push_back(candidates[best]);
        if (result.size() == limit) break;
        float best_score = -std::numeric_limits<float>::infinity();
        size_t next = 0;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (selected[i]) continue;
            redundancy[i] = std::max(redundancy[i], cosineSimilarity(
                candidates[i].tool.embedding, candidates[best].tool.embedding));
            const float score = config.lambda * candidates[i].similarity -
                                (1.0f - config.lambda) * redundancy[i];
            // 同分沿用候选池的相似度、工具名顺序，保证结果可复现。
            if (score > best_score) {
                best_score = score;
                next = i;
            }
        }
        best = next;
    }
    return result;
}

float VectorIndex::cosineSimilarity(
    const std::vector<float>& a,
    const std::vector<float>& b) {
    
    if (a.size() != b.size() || a.empty()) {
        return 0.0f;
    }
    
    double dot_product = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;
    
    for (size_t i = 0; i < a.size(); ++i) {
        dot_product += static_cast<double>(a[i]) * b[i];
        norm_a += static_cast<double>(a[i]) * a[i];
        norm_b += static_cast<double>(b[i]) * b[i];
    }
    
    if (norm_a == 0.0 || norm_b == 0.0 || !std::isfinite(dot_product) ||
        !std::isfinite(norm_a) || !std::isfinite(norm_b)) {
        return 0.0f;
    }
    
    return static_cast<float>(std::clamp(
        dot_product / (std::sqrt(norm_a) * std::sqrt(norm_b)), -1.0, 1.0));
}

bool VectorIndex::saveToFile(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        json tools_array = json::array();
        
        for (const auto& pair : tools_) {
            const IndexedTool& tool = pair.second;
            
            json tool_json = {
                {"name", tool.name},
                {"description", tool.description},
                {"input_schema", tool.input_schema},
                {"embedding", tool.embedding},
                {"created_at", tool.created_at},
                {"updated_at", tool.updated_at}
            };
            
            tools_array.push_back(tool_json);
        }
        
        json index_json = {
            {"version", version_},
            {"model", "text-embedding-v2"},
            {"dimension", tools_.empty() ? 1536 : 
                (tools_.begin()->second.embedding.empty() ? 1536 : 
                 static_cast<int>(tools_.begin()->second.embedding.size()))},
            {"tools", tools_array}
        };
        
        std::ofstream file(path);
        if (!file.is_open()) {
            LOG_ERROR("Failed to open file for writing: " + path);
            return false;
        }
        
        file << index_json.dump(2);
        file.close();
        
        LOG_INFO("Saved vector index to: " + path + " (" + 
                std::to_string(tools_.size()) + " tools)");
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to save vector index: " + std::string(e.what()));
        return false;
    }
}

bool VectorIndex::loadFromFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            LOG_WARN("Index file not found: " + path);
            return false;
        }
        
        json index_json = json::parse(file);
        file.close();
        
        // 读取版本
        if (index_json.contains("version")) {
            version_ = index_json["version"].get<std::string>();
        }
        
        // 清空现有数据
        tools_.clear();
        
        // 加载工具
        if (index_json.contains("tools")) {
            for (const auto& tool_json : index_json["tools"]) {
                IndexedTool tool;
                tool.name = tool_json.value("name", "");
                tool.description = tool_json.value("description", "");
                tool.input_schema = tool_json.value("input_schema", "");
                tool.created_at = tool_json.value("created_at", 0);
                tool.updated_at = tool_json.value("updated_at", 0);
                
                if (tool_json.contains("embedding")) {
                    for (const auto& val : tool_json["embedding"]) {
                        tool.embedding.push_back(val.get<float>());
                    }
                }
                
                if (!tool.name.empty()) {
                    tools_[tool.name] = tool;
                }
            }
        }
        
        LOG_INFO("Loaded vector index from: " + path + " (" + 
                std::to_string(tools_.size()) + " tools)");
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load vector index: " + std::string(e.what()));
        return false;
    }
}

size_t VectorIndex::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.size();
}

void VectorIndex::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    tools_.clear();
}

} // namespace rag
} // namespace mcp
} // namespace agent_rpc
