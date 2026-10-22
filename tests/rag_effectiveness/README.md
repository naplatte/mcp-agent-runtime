# RAG 工具筛选效果评测

在项目根目录运行。适用于 Linux / WSL，Python 3.10+，不需要额外 Python 包。
脚本自动构建当前项目的 MCP 服务端、天气/日志插件及 C++ RAG 桥接程序。

## 1. 先确认评测程序能运行（离线，无需密钥）

```bash
python3 tests/rag_effectiveness/run.py --self-test
```

成功时最后显示：`[自检通过]`。这验证真实 MCP 工具发现、项目的 C++ 检索/缓存、
空向量和 API 错误处理、评分与报告流程。Embedding 和模型接口在本机模拟，
**自检结果不是 RAG 效果数据，也不能写成真实准确率或 token 优化结果。**

若构建缺少依赖，Ubuntu 可安装：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libcurl4-openssl-dev libjsoncpp-dev nlohmann-json3-dev
```

## 2. 使用真实 API 试跑

```bash
read -rsp '输入百炼API Key: ' DASHSCOPE_API_KEY
export DASHSCOPE_API_KEY
printf '\n'
python3 tests/rag_effectiveness/run.py --live --quick
```

密钥只从环境变量读取，不写入源代码、报告或命令行参数。
缺少密钥时退出码是 **77（未运行）**，不是通过。

快速模式使用 16 个开发问题和 12 个测试问题，对比基线与 50 工具目录，
默认进行 48 次真实模型请求。用于先检查 API 兼容性与报告输出。

## 3. 正式对照

```bash
python3 tests/rag_effectiveness/run.py --live --repeats 3
```

正式模式有 80 个问题：15 个开发问题用于选择 k，65 个测试问题用于最终对照。
每个目录比较全量工具与 RAG 两组；3 个目录、重复 3 次，共 1170 次模型请求。
想减少调用次数，可用 `--repeats 1`（默认），共 390 次模型请求。

每次运行只产出一个结果文件（中文、精简，可直接拿去讲）：

```text
tests/rag_effectiveness/results/live-日期-随机编号/
  report.md          题目规模、检索命中率、全量 vs RAG 对照、结论、失败案例、局限
```

构建日志和 C++/MCP 诊断日志写在 `build/rag_effectiveness/logs/<运行编号>/`，不进结果目录，
排查时再看。结果目录默认被 Git 忽略。

## 4. 报告怎么看

1. **状态**：`完成` 表示请求都收到了，不代表 RAG 一定更好；有接口失败会标 `不完整`。
2. **第 2 节检索命中率**：题需要的工具是否全部进入候选；多工具题只命中一个不算过。
3. **第 3 节两组对照**：`调用正确率` 要求工具集合、参数类型和关键值都对；`工具选择正确率` 只看集合。
4. **第 4 节结论**：token 降幅来自接口真实 `usage.prompt_tokens`；正确率按百分比点比较。
5. **第 5 节失败案例**：看预期工具与实调用，区分“检索没召回”和“模型选错”。
6. **第 6 节局限**：模拟目录、单模型、单 embedding，结论只对这组题成立。

k 在开发集上按“完整命中率、平均召回率、小 k 优先”选择，测试集不参与。
基线（4 工具）目录只评测它支持的题，其余题跳过并在第 1 节注明。

## 5. 工具数量如何增加

- **基线：4 个真实工具**。单独启动 MCP Server，通过项目 `MCPAgentIntegration` 发现
  天气插件的 `get_weather`，以及日志插件的 `tail_log`、`analyze_log`、`summarize_errors`。
- **50 / 100 工具目录**：保留这 4 个工具的真实定义，追加 130 个可区分的模拟定义。
  其中 12 个是贴着真实工具造的近似能力（历史天气、空气质量、日志正则、全文读取、计数等），
  34 个是热门 MCP 服务形态的空壳工具（GitHub、Slack、Notion、文件系统、Postgres、
  浏览器、Web 搜索、Sentry、Jira、Linear、Gmail、Google Calendar、Drive、Memory），
  其余 84 个是 28 个业务域 × list/get/create 的批量生成项。
  名称和功能说明各不相同，参数按能力定义，不是给同一个工具加编号。
- 50 工具目录 = 4 真实 + 12 近似 + 34 空壳；100 工具目录在前者基础上再加 50 个批量生成项。
- 基线（4 工具）目录只能评测所需工具都在其中的 34 题，其余 42 题跳过并记入报告，
  不记成失败；因此 baseline 与 50/100 的准确率不在同一题集上，只在同目录内两组之间比较。
- 每个目录中，全量组与 RAG 组使用同一批定义；50 工具目录是 100 工具目录的前缀。
- 模拟工具只有元数据，**不是新增的真实功能插件**。本评测不执行它们，也不执行天气等业务API，
  以免外部天气服务或文件状态干扰“检索与工具选择”的评价。
- 部分测试题的预期工具就是这些空壳工具（跨应用场景）。评分只比对 `tool_calls` 的名称、
  参数类型和关键参数值，不执行工具，因此空壳不会让这类题失效；它们用来测跨域检索与
  细粒度区分（例如 `count_log_lines` 与 `analyze_log`、`fs_read_file` 与 `read_entire_log`）。
- 检索直接复用项目的 `ToolRetriever / EmbeddingService / VectorIndex / EmbeddingCache`，
  候选定义复用 `MCPAgentIntegration::toFunctionCallingFormat`。Python 只负责目录、调度、评分和展示。

数据集覆盖中文改写、英文、多工具需求、无需工具和近似功能边界，
共 80 题（15 开发 / 65 测试），三类比例约 5:3:2：40 单工具、24 多工具、16 无需工具。
参数提供足够信息，例如天气问题直接给出经纬度；评分接受等价坐标字符串及中英文城市名。
标签和生成规则见 `dataset.py`，结果保存实际使用的所有工具定义。

这些样本是人工构造的有限测试集，扩展目录是模拟规模。
结论只能描述这组测试中的表现，不代表生产准确率。当前索引阈值未达标时仍返回最佳工具，
所以无需工具的 16 道问题也用于观察模型会不会错误调用。

## 6. 可选参数

```bash
python3 tests/rag_effectiveness/run.py --live --repeats 1 --threshold 0.3 --k-values 1,3,5
```

- `--model`：默认 `qwen-plus`。
- `--embedding-model`、`--dimension`：默认 `text-embedding-v2`、1536，修改时确保实际向量维度一致。
- `--sizes 50,100`：额外目录规模；基线自动加入。
- `--chat-url`、`--embedding-url`：自定义兼容接口；默认使用阿里百炼中国区。
- `--output`：结果根目录；每次仍创建独立子目录。
- `--jobs`：构建并行数，默认2。

工具目录最大100个，k 必须小于实际工具总数；基线自动忽略 k=5。
更改阈值或描述后，应使用开发集选择参数并保留最终测试集。
