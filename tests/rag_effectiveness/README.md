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

快速模式使用 6 个开发问题和 6 个测试问题，对比基线与 50 工具目录，
默认进行 24 次真实模型请求。用于先检查 API 兼容性与报告输出。

## 3. 正式对照

```bash
python3 tests/rag_effectiveness/run.py --live --repeats 3
```

正式模式有 30 个问题：6 个开发问题用于选择 k，24 个测试问题用于最终对照。
每个目录比较全量工具与 RAG 两组；3 个目录、重复 3 次，共 432 次模型请求。
想减少调用次数，可用 `--repeats 1`（默认），共 144 次模型请求。

每次运行生成独立结果目录，终端会显示 `report.md` 的准确位置：

```text
tests/rag_effectiveness/results/live-日期-随机编号/
  report.md          中文结果、指标定义、失败案例
  summary.json       完整目录、标签、配置、统计及响应
  details.jsonl      每次模型请求的评分、usage、时间与响应
  progress.jsonl     已完成请求的即时记录，便于中断排查
  tool_index.json    项目 C++ 索引保存的工具向量
  backend.log        C++ / MCP 诊断日志
  build.log          构建日志
```

结果目录默认被 Git 忽略。

## 4. 结果怎么看

1. **先看状态**：`完成`表示评测收集完成，不表示 RAG 一定更好。API 异常会标记不完整，
   索引/检索失败会中止，不回退到全量工具冒充 RAG。
2. **检索完整命中率**：问题所需的所有工具是否进入候选；多工具问题不能仅命中一个就算通过。
   表中同时给出平均召回率。无需工具的问题不参与这两项分母。
3. **工具选择正确率 / 调用正确率**：前者只看工具集合；后者还要求参数符合 schema 和题意。
   多调用无关工具、重复调用、漏工具、参数类型或关键值错误都会判错。
4. **平均输入 token**：来自模型接口真实 `usage.prompt_tokens`；不是工具数量比例或 JSON 字符数。
5. **总耗时**：检索 + 模型首轮工具选择请求。分别记录第一次查询和同一问题缓存命中的查询。
   不包括天气/文件业务执行及最终答案生成。
6. **失败案例**：看问题、预期工具、候选工具、实际调用和失败原因，区分检索漏召回与模型选错。

两组使用同一模型、提示词、温度、问题、原始工具定义；请求先后交替。
k 在开发集上按“完整命中率、平均召回率、小 k 优先”的顺序选择，测试集不参与选择。
首轮查询前清空查询缓存，重载已经建好的工具向量；每个测试问题先测选定 k 的冷检索，
再立即重复检索测缓存命中。其他 k 的候选利用同一查询向量做额外检索，不重复调用 Embedding。
从第二轮模型对照起，用实测的缓存命中检索耗时计算总耗时。

索引扩展是一次性成本，单独报告。报告也显示向量缓存未命中次数，
但项目 Embedding 封装没有返回完整厂商计费 usage；**不能据此宣称货币总成本下降**。
模型侧缓存用量保留在原始 usage 中，网络波动仍可能影响时间。

## 5. 工具数量如何增加

- **基线：4 个真实工具**。单独启动 MCP Server，通过项目 `MCPAgentIntegration` 发现
  天气插件的 `get_weather`，以及日志插件的 `tail_log`、`analyze_log`、`summarize_errors`。
- **50 / 100 工具目录**：保留这 4 个工具的真实定义，追加 96 个可区分的模拟定义。
  包含历史天气、空气质量、气象预警、日志正则搜索、全文读取、计数等近似能力，
  以及邮件、日程、工单、订单、Git、数据库等不同业务的列表/详情/创建能力。
  名称和功能说明各不相同，参数按能力定义，不是给同一个工具加编号。
- 每个目录中，全量组与 RAG 组使用同一批定义；50 工具目录是 100 工具目录的前缀。
- 模拟工具只有元数据，**不是新增的真实功能插件**。本评测不执行它们，也不执行天气等业务API，
  以免外部天气服务或文件状态干扰“检索与工具选择”的评价。
- 检索直接复用项目的 `ToolRetriever / EmbeddingService / VectorIndex / EmbeddingCache`，
  候选定义复用 `MCPAgentIntegration::toFunctionCallingFormat`。Python 只负责目录、调度、评分和展示。

数据集覆盖中文改写、英文、多工具需求、无需工具和近似功能边界。
参数提供足够信息，例如天气问题直接给出经纬度；评分接受等价坐标字符串及中英文城市名。
标签和生成规则见 `dataset.py`，结果保存实际使用的所有工具定义。

这些样本是人工构造的有限测试集，扩展目录是模拟规模。
结论只能描述这组测试中的表现，不代表生产准确率。当前索引阈值未达标时仍返回最佳工具，
所以无需工具的两道问题也用于观察模型会不会错误调用。

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
