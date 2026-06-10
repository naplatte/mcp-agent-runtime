#!/bin/bash
# MCP 项目测试入口
# 支持四种测试模式:
#   1. AI 集成测试 (默认)     - 使用阿里百炼 AI 模型测试完整 Agent 链路 (需要 API Key)
#   2. 基础功能测试            - 无需 API Key，用 bash 直接测 MCP Server 协议
#   3. C++ 自动化测试          - 使用项目自身的 mcp_client 自动化测试 server + plugins
#   4. C++ 交互模式            - 手动输入命令调用工具，实时看效果

set -e # 任何命令出错都会停止执行

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=============================================="
echo "  MCP 项目测试"
echo "=============================================="
echo ""
echo "  1) AI 集成测试     - 使用百炼 AI 模型 + MCP 工具调用 (需要 API Key)"
echo "  2) 基础功能测试     - 仅测试 MCP Server 协议和工具 (无需 API Key)"
echo "  3) C++ 自动化测试   - 使用项目自身 mcp_client 自动测试 (无需 API Key)"
echo "  4) C++ 交互模式    - 手动输入命令调用工具，实时看效果"
echo ""
read -rp "选择测试模式 [默认 1]: " MODE
MODE=${MODE:-1}

if [ "$MODE" = "2" ]; then
    # ============================
    # 基础功能测试 (bash)
    # ============================
    SERVER="$PROJECT_ROOT/build/mcp_server/mcp_server"
    PLUGINS="$PROJECT_ROOT/build/mcp_server/plugins"
    LOGS="$PROJECT_ROOT/build/test_logs"
    mkdir -p "$LOGS"

    if [ ! -x "$SERVER" ]; then
        echo "错误: 未找到 mcp_server: $SERVER"
        echo "请先构建: mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
        exit 1
    fi

    INIT='{"jsonrpc":"2.0","method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}},"id":"init"}'

    run() { echo -e "${INIT}\n$1" | timeout 15 "$SERVER" -p "$PLUGINS" -l "$LOGS" 2>/dev/null || true; }
    run1() { echo -e "$1" | timeout 10 "$SERVER" -p "$PLUGINS" -l "$LOGS" 2>/dev/null || true; }

    PASS=0; FAIL=0
    check() {
        local name="$1" ok="$2"
        if [ "$ok" = "1" ]; then PASS=$((PASS+1)); echo -e "  \033[0;32m[PASS]\033[0m $name"
        else FAIL=$((FAIL+1)); echo -e "  \033[0;31m[FAIL]\033[0m $name"; fi
    }

    echo ""
    echo "运行基础功能测试..."
    echo ""

    R=$(run1 "$INIT")
    echo "$R" | grep -q '"serverInfo"' && check "initialize" 1 || check "initialize" 0

    R=$(run '{"jsonrpc":"2.0","method":"ping","params":{},"id":"p"}')
    echo "$R" | grep -q '"id":"p"' && check "ping" 1 || check "ping" 0

    R=$(run '{"jsonrpc":"2.0","method":"tools/list","params":{},"id":"tl"}')
    echo "$R" | grep -q '"tools"' && check "tools/list" 1 || check "tools/list" 0

    R=$(run '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"calculator","arguments":{"expression":"1+2"}},"id":"c"}')
    L=$(echo "$R" | grep '"id":"c"')
    echo "$L" | grep -q '"isError":false\|"result"' && ! echo "$L" | grep -q '"isError":true' && check "calculator" 1 || check "calculator" 0

    R=$(run '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"sleep","arguments":{"milliseconds":100}},"id":"s"}')
    L=$(echo "$R" | grep '"id":"s"')
    echo "$L" | grep -q '"result"' && ! echo "$L" | grep -q '"isError":true' && check "sleep" 1 || check "sleep" 0

    R=$(run '{"jsonrpc":"2.0","method":"tools/call","params":{"name":"no_tool","arguments":{}},"id":"e"}')
    L=$(echo "$R" | grep '"id":"e"')
    echo "$L" | grep -q '"isError":true' && check "error handling" 1 || check "error handling" 0

    echo ""
    TOTAL=$((PASS+FAIL))
    echo -e "总计: $TOTAL  \033[0;32m通过: $PASS\033[0m  \033[0;31m失败: $FAIL\033[0m"
    [ "$FAIL" -eq 0 ] && echo -e "\033[0;32m所有测试通过!\033[0m" || echo -e "\033[0;31m有 $FAIL 个测试失败\033[0m"
    exit "$FAIL"

elif [ "$MODE" = "3" ]; then
    # ============================
    # C++ 集成测试
    # ============================
    SERVER="$PROJECT_ROOT/build/mcp_server/mcp_server"
    PLUGINS="$PROJECT_ROOT/build/mcp_server/plugins"
    TEST_BIN="$PROJECT_ROOT/build/tests/cpp/test_mcp_integration"

    if [ ! -x "$TEST_BIN" ]; then
        echo "错误: 未找到测试程序: $TEST_BIN"
        echo "请先构建: mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
        exit 1
    fi

    if [ ! -x "$SERVER" ]; then
        echo "错误: 未找到 mcp_server: $SERVER"
        echo "请先构建: mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
        exit 1
    fi

    exec "$TEST_BIN" "$SERVER" "$PLUGINS"

elif [ "$MODE" = "4" ]; then
    # ============================
    # C++ 交互模式
    # ============================
    SERVER="$PROJECT_ROOT/build/mcp_server/mcp_server"
    PLUGINS="$PROJECT_ROOT/build/mcp_server/plugins"
    TEST_BIN="$PROJECT_ROOT/build/tests/cpp/test_mcp_integration"

    if [ ! -x "$TEST_BIN" ]; then
        echo "错误: 未找到测试程序: $TEST_BIN"
        echo "请先构建: mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
        exit 1
    fi

    if [ ! -x "$SERVER" ]; then
        echo "错误: 未找到 mcp_server: $SERVER"
        echo "请先构建: mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
        exit 1
    fi

    exec "$TEST_BIN" "$SERVER" "$PLUGINS" -i

else
    # ============================
    # AI 集成测试
    # ============================
    exec python3 "$SCRIPT_DIR/python/test_ai_mcp.py"
fi
