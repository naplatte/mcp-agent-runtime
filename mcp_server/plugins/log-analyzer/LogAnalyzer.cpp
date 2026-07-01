/**
 * @file LogAnalyzer.cpp
 * @brief MCP Log Analyzer Plugin
 */

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "PluginAPI.h"
#include "json.hpp"

using json = nlohmann::json;

namespace {

constexpr int kDefaultTailLines = 50;
constexpr int kDefaultAnalyzeLines = 200;
constexpr int kMaxReadLines = 2000;
constexpr int kMaxReturnedMatches = 50;

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool containsCaseInsensitive(const std::string& text, const std::string& keyword) {
    return toLower(text).find(toLower(keyword)) != std::string::npos;
}

int getIntArg(const json& args, const std::string& key, int default_value, int min_value, int max_value) {
    int value = default_value;
    if (args.contains(key) && !args[key].is_null()) {
        value = args[key].get<int>();
    }
    if (value < min_value) value = min_value;
    if (value > max_value) value = max_value;
    return value;
}

std::string getPathArg(const json& args) {
    if (!args.contains("path") || !args["path"].is_string()) {
        throw std::runtime_error("missing required string argument: path");
    }
    return args["path"].get<std::string>();
}

void validateLogPath(const std::string& path) {
    if (path.empty()) {
        throw std::runtime_error("path is empty");
    }

    std::filesystem::path log_path(path);
    const std::string normalized = log_path.lexically_normal().string();
    if (normalized.find("..") != std::string::npos) {
        throw std::runtime_error("path must not contain '..'");
    }

    const std::string ext = toLower(log_path.extension().string());
    if (ext != ".log" && ext != ".txt" && ext != ".out" && ext != ".err") {
        throw std::runtime_error("only .log, .txt, .out and .err files are allowed");
    }

    std::error_code ec;
    if (!std::filesystem::exists(log_path, ec) || !std::filesystem::is_regular_file(log_path, ec)) {
        throw std::runtime_error("log file does not exist or is not a regular file");
    }
}

std::vector<std::string> readLastLines(const std::string& path, int line_count) {
    validateLogPath(path);

    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("failed to open log file");
    }

    std::deque<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
        if (static_cast<int>(lines.size()) > line_count) {
            lines.pop_front();
        }
    }

    return {lines.begin(), lines.end()};
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::ostringstream oss;
    for (size_t i = 0; i < lines.size(); ++i) {
        oss << lines[i];
        if (i + 1 < lines.size()) {
            oss << "\n";
        }
    }
    return oss.str();
}

json makeResponse(const std::string& text, bool is_error = false) {
    json content;
    content["type"] = "text";
    content["text"] = text;

    json response;
    response["content"] = json::array();
    response["content"].push_back(content);
    response["isError"] = is_error;
    return response;
}

std::string tailLog(const json& args) {
    const std::string path = getPathArg(args);
    const int lines = getIntArg(args, "lines", kDefaultTailLines, 1, kMaxReadLines);
    const auto tail = readLastLines(path, lines);

    std::ostringstream oss;
    oss << "tail_log: " << path << "\n";
    oss << "returned_lines: " << tail.size() << "\n\n";
    oss << joinLines(tail);
    return oss.str();
}

std::string analyzeLog(const json& args) {
    const std::string path = getPathArg(args);
    const std::string keyword = args.value("keyword", std::string());
    const int max_lines = getIntArg(args, "max_lines", kDefaultAnalyzeLines, 1, kMaxReadLines);
    const auto lines = readLastLines(path, max_lines);

    std::vector<std::string> matches;
    int warn_count = 0;
    int error_count = 0;

    for (const auto& line : lines) {
        if (containsCaseInsensitive(line, "WARN")) ++warn_count;
        if (containsCaseInsensitive(line, "ERROR")) ++error_count;

        const bool matched = keyword.empty()
            ? (containsCaseInsensitive(line, "WARN") || containsCaseInsensitive(line, "ERROR"))
            : containsCaseInsensitive(line, keyword);
        if (matched && static_cast<int>(matches.size()) < kMaxReturnedMatches) {
            matches.push_back(line);
        }
    }

    std::ostringstream oss;
    oss << "analyze_log: " << path << "\n";
    oss << "scanned_lines: " << lines.size() << "\n";
    oss << "keyword: " << (keyword.empty() ? "<WARN|ERROR>" : keyword) << "\n";
    oss << "error_count: " << error_count << "\n";
    oss << "warn_count: " << warn_count << "\n";
    oss << "returned_matches: " << matches.size() << "\n\n";
    oss << joinLines(matches);
    return oss.str();
}

std::string summarizeErrors(const json& args) {
    const std::string path = getPathArg(args);
    const int max_lines = getIntArg(args, "max_lines", 500, 1, kMaxReadLines);
    const auto lines = readLastLines(path, max_lines);

    int warn_count = 0;
    int error_count = 0;
    std::vector<std::string> samples;
    std::map<std::string, int> buckets;

    for (const auto& line : lines) {
        const bool is_error = containsCaseInsensitive(line, "ERROR");
        const bool is_warn = containsCaseInsensitive(line, "WARN");
        if (!is_error && !is_warn) {
            continue;
        }

        if (is_error) ++error_count;
        if (is_warn) ++warn_count;

        std::string key = is_error ? "ERROR" : "WARN";
        const auto pos = line.find(']');
        if (pos != std::string::npos && pos + 1 < line.size()) {
            key += ": " + line.substr(pos + 1, 80);
        }
        buckets[key]++;

        if (samples.size() < 20) {
            samples.push_back(line);
        }
    }

    std::ostringstream oss;
    oss << "summarize_errors: " << path << "\n";
    oss << "scanned_lines: " << lines.size() << "\n";
    oss << "error_count: " << error_count << "\n";
    oss << "warn_count: " << warn_count << "\n\n";
    std::vector<std::pair<std::string, int>> sorted_buckets(buckets.begin(), buckets.end());
    std::sort(sorted_buckets.begin(), sorted_buckets.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.second > rhs.second;
    });

    oss << "top_buckets:\n";
    int emitted = 0;
    for (const auto& [bucket, count] : sorted_buckets) {
        if (emitted++ >= 10) break;
        oss << "- " << count << " x " << bucket << "\n";
    }

    oss << "\nrecent_samples:\n";
    oss << joinLines(samples);
    return oss.str();
}

} // namespace

static PluginTool methods[] = {
    {
        "tail_log",
        "Return the last N lines of a log file.",
        R"({
            "type": "object",
            "properties": {
                "path": { "type": "string", "description": "Path to a .log, .txt, .out or .err file." },
                "lines": { "type": "integer", "minimum": 1, "maximum": 2000, "default": 50 }
            },
            "required": ["path"],
            "additionalProperties": false
        })"
    },
    {
        "analyze_log",
        "Filter recent log lines by keyword, or WARN/ERROR when keyword is empty.",
        R"({
            "type": "object",
            "properties": {
                "path": { "type": "string", "description": "Path to a .log, .txt, .out or .err file." },
                "keyword": { "type": "string", "description": "Optional case-insensitive keyword." },
                "max_lines": { "type": "integer", "minimum": 1, "maximum": 2000, "default": 200 }
            },
            "required": ["path"],
            "additionalProperties": false
        })"
    },
    {
        "summarize_errors",
        "Summarize WARN and ERROR lines from recent log lines.",
        R"({
            "type": "object",
            "properties": {
                "path": { "type": "string", "description": "Path to a .log, .txt, .out or .err file." },
                "max_lines": { "type": "integer", "minimum": 1, "maximum": 2000, "default": 500 }
            },
            "required": ["path"],
            "additionalProperties": false
        })"
    }
};

const char* GetNameImpl() { return "log-analyzer-tools"; }
const char* GetVersionImpl() { return "1.0.0"; }
PluginType GetTypeImpl() { return PLUGIN_TYPE_TOOLS; }
int InitializeImpl() { return 1; }

char* HandleRequestImpl(const char* req) {
    json response;

    try {
        auto request = json::parse(req);
        const std::string tool_name = request["params"]["name"].get<std::string>();
        const json args = request["params"].value("arguments", json::object());

        if (tool_name == "tail_log") {
            response = makeResponse(tailLog(args));
        } else if (tool_name == "analyze_log") {
            response = makeResponse(analyzeLog(args));
        } else if (tool_name == "summarize_errors") {
            response = makeResponse(summarizeErrors(args));
        } else {
            response = makeResponse("Unknown tool: " + tool_name, true);
        }
    } catch (const std::exception& e) {
        response = makeResponse(std::string("Error: ") + e.what(), true);
    }

    const std::string result = response.dump();
    char* buffer = new char[result.length() + 1];
    std::strcpy(buffer, result.c_str());
    return buffer;
}

void ShutdownImpl() {}

int GetToolCountImpl() {
    return sizeof(methods) / sizeof(methods[0]);
}

const PluginTool* GetToolImpl(int index) {
    if (index < 0 || index >= GetToolCountImpl()) return nullptr;
    return &methods[index];
}

static PluginAPI plugin = {
    GetNameImpl,
    GetVersionImpl,
    GetTypeImpl,
    InitializeImpl,
    HandleRequestImpl,
    ShutdownImpl,
    GetToolCountImpl,
    GetToolImpl,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

extern "C" PLUGIN_API PluginAPI* CreatePlugin() {
    return &plugin;
}

extern "C" PLUGIN_API void DestroyPlugin(PluginAPI*) {}
