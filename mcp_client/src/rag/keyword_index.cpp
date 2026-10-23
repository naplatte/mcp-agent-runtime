#include "agent_rpc/mcp/rag/keyword_index.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace agent_rpc::mcp::rag {
namespace {
bool asciiAlnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

std::string lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}

void appendTerm(std::vector<std::string>& result, const std::string& term) {
    static const std::unordered_set<std::string> stopwords = {
        "a", "an", "the", "of", "to", "in", "on", "for", "and", "or", "with",
        "by", "from", "is", "are", "as", "at", "through", "this", "that",
        "tool", "description", "parameters", "required", "optional"
    };
    if (!term.empty() && !stopwords.count(term)) result.push_back(term);
}

// 通用词语对应只用于本地关键词查询；不读取题目标签，不修改工具描述/向量。
std::unordered_set<std::string> queryTerms(const std::string& query) {
    auto tokens = KeywordIndex::tokenize(query);
    std::unordered_set<std::string> terms(tokens.begin(), tokens.end());
    static const std::vector<std::pair<std::string, std::string>> aliases = {
        {"邮件", "email mail"}, {"邮箱", "email mail"},
        {"发送", "send post"}, {"发给", "send"}, {"发到", "send post"},
        {"日程", "calendar event"}, {"日历", "calendar event"}, {"会议", "meeting event"},
        {"创建", "create"}, {"新建", "create"}, {"添加", "add append"}, {"追加", "append"},
        {"搜索", "search"}, {"搜一下", "search"}, {"查找", "search find"},
        {"查询", "query search"}, {"读取", "read"}, {"读一下", "read get"},
        {"列出", "list"}, {"清单", "list"}, {"目录", "directory"},
        {"文件夹", "directory folder"}, {"文件", "file"},
        {"网页", "page web"}, {"抓取", "fetch"}, {"截图", "screenshot"},
        {"天气", "weather"}, {"日志", "log"}, {"最后", "last tail"}, {"末尾", "tail"},
        {"行数", "count lines"}, {"多少行", "count lines"},
        {"表结构", "describe table schema"}, {"数据库", "database"},
        {"消息", "message"}, {"频道", "channel"}, {"页面", "page"},
        {"问题", "issue"}, {"仓库", "repository repo"}, {"代码", "code"}
    };
    for (const auto& [phrase, expansion] : aliases) {
        if (query.find(phrase) == std::string::npos) continue;
        for (const auto& token : KeywordIndex::tokenize(expansion)) terms.insert(token);
    }
    return terms;
}
} // namespace

std::vector<std::string> KeywordIndex::tokenize(const std::string& text) {
    std::vector<std::string> result;
    std::string previous_han;
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (asciiAlnum(c) || c == '#' || c == '_') {
            previous_han.clear();
            const size_t start = i++;
            while (i < text.size()) {
                const auto next = static_cast<unsigned char>(text[i]);
                if (!asciiAlnum(next) && next != '_' && next != '-' && next != '.') break;
                ++i;
            }
            auto original = text.substr(start, i - start);
            while (!original.empty() && (original.back() == '.' || original.back() == '-')) original.pop_back();
            const auto full = lower(original);
            appendTerm(result, full);
            std::unordered_set<std::string> emitted = {full};
            // GitHub_createIssue 同时保留 github、createissue 和 camelCase 子词。
            size_t segment = 0;
            for (size_t j = 0; j <= original.size(); ++j) {
                if (j < original.size() && asciiAlnum(static_cast<unsigned char>(original[j]))) continue;
                const auto word = lower(original.substr(segment, j - segment));
                if (emitted.insert(word).second) appendTerm(result, word);
                segment = j + 1;
            }
            size_t part = 0;
            for (size_t j = 0; j <= original.size(); ++j) {
                const bool delimiter = j == original.size() || !asciiAlnum(static_cast<unsigned char>(original[j]));
                const bool camel = j > part && j < original.size() && original[j] >= 'A' &&
                                   original[j] <= 'Z' && original[j - 1] >= 'a' && original[j - 1] <= 'z';
                if (!delimiter && !camel) continue;
                const auto word = lower(original.substr(part, j - part));
                if (emitted.insert(word).second) appendTerm(result, word);
                part = delimiter ? j + 1 : j;
            }
            continue;
        }
        // 按 UTF-8 字符切中文单字/相邻双字，不把一个汉字拆成字节。
        const size_t bytes = c < 0x80 ? 1 : (c >= 0xc2 && c < 0xe0 ? 2 :
                             (c < 0xf0 && c >= 0xe0 ? 3 : (c >= 0xf0 && c <= 0xf4 ? 4 : 0)));
        uint32_t point = bytes ? c & (0x7f >> bytes) : 0;
        bool valid = bytes && i + bytes <= text.size();
        for (size_t j = 1; valid && j < bytes; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            valid = (next & 0xc0) == 0x80;
            point = (point << 6) | (next & 0x3f);
        }
        const bool han = valid && ((point >= 0x3400 && point <= 0x9fff) ||
                         (point >= 0xf900 && point <= 0xfaff) || (point >= 0x20000 && point <= 0x2fa1f));
        if (han) {
            const auto current = text.substr(i, bytes);
            result.push_back(current);
            if (!previous_han.empty()) result.push_back(previous_han + current);
            previous_han = current;
        } else previous_han.clear();
        i += valid ? bytes : 1;
    }
    return result;
}

void KeywordIndex::addTool(const std::string& name, const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    removeUnlocked(name);
    Document document;
    for (const auto& token : tokenize(text)) ++document.terms[token];
    for (const auto& [term, frequency] : document.terms) {
        document.length += frequency;
        postings_[term][name] = frequency;
    }
    total_length_ += document.length;
    documents_.emplace(name, std::move(document));
}

bool KeywordIndex::removeUnlocked(const std::string& name) {
    const auto found = documents_.find(name);
    if (found == documents_.end()) return false;
    for (const auto& [term, frequency] : found->second.terms) {
        (void)frequency;
        auto posting = postings_.find(term);
        posting->second.erase(name);
        if (posting->second.empty()) postings_.erase(posting);
    }
    total_length_ -= found->second.length;
    documents_.erase(found);
    return true;
}

bool KeywordIndex::removeTool(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return removeUnlocked(name);
}

void KeywordIndex::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    documents_.clear();
    postings_.clear();
    total_length_ = 0;
}

std::vector<KeywordMatch> KeywordIndex::search(const std::string& query, int top_k) const {
    if (top_k <= 0) return {};
    const auto terms = queryTerms(query);
    std::lock_guard<std::mutex> lock(mutex_);
    if (total_length_ == 0) return {};
    const double average = static_cast<double>(total_length_) / documents_.size();
    std::unordered_map<std::string, double> scores;
    // 对同一查询按固定词序累加，避免浮点误差受哈希迭代顺序影响。
    std::vector<std::string> ordered(terms.begin(), terms.end());
    std::sort(ordered.begin(), ordered.end());
    for (const auto& term : ordered) {
        const auto found = postings_.find(term);
        if (found == postings_.end()) continue;
        const double idf = std::log(1.0 + (documents_.size() - found->second.size() + 0.5) /
                                          (found->second.size() + 0.5));
        for (const auto& [name, frequency] : found->second) {
            const double length = documents_.at(name).length;
            scores[name] += idf * frequency * 2.2 / (frequency + 1.2 * (0.25 + 0.75 * length / average));
        }
    }
    std::vector<KeywordMatch> result;
    for (const auto& [name, value] : scores) result.push_back({name, value});
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.score != b.score ? a.score > b.score : a.name < b.name;
    });
    if (result.size() > static_cast<size_t>(top_k)) result.resize(top_k);
    return result;
}

std::vector<std::string> reciprocalRankFusion(const std::vector<std::string>& dense,
    const std::vector<std::string>& keywords, int top_k, int rank_constant) {
    if (top_k <= 0 || rank_constant < 0) throw std::invalid_argument("Invalid RRF configuration");
    struct Entry { double score = 0; size_t dense_rank = std::numeric_limits<size_t>::max(); };
    std::unordered_map<std::string, Entry> scores;
    for (size_t route = 0; route < 2; ++route) {
        const auto& list = route == 0 ? dense : keywords;
        std::unordered_set<std::string> seen;
        size_t rank = 0;
        for (const auto& name : list) {
            if (!seen.insert(name).second) continue;
            ++rank;
            auto& entry = scores[name];
            entry.score += 1.0 / (static_cast<double>(rank_constant) + rank);
            if (route == 0) entry.dense_rank = rank;
        }
    }
    std::vector<std::string> result;
    for (const auto& [name, entry] : scores) { (void)entry; result.push_back(name); }
    std::sort(result.begin(), result.end(), [&](const auto& a, const auto& b) {
        const auto& x = scores.at(a);
        const auto& y = scores.at(b);
        if (x.score != y.score) return x.score > y.score;
        return x.dense_rank != y.dense_rank ? x.dense_rank < y.dense_rank : a < b;
    });
    if (result.size() > static_cast<size_t>(top_k)) result.resize(top_k);
    return result;
}
} // namespace agent_rpc::mcp::rag
