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

快速模式只跑 12 道抽样题，只测 50 工具目录，默认 24 次真实模型请求。
用于先检查 API 兼容性与报告输出。

## 3. 正式对照

```bash
python3 tests/rag_effectiveness/run.py --live --repeats 3
```
80 个问题全部参与，不再划分开发/测试集：目录为 50 / 100 / 300 三档，
每档比较全量工具与 RAG 两组；重复 1 次共 480 次模型请求，重复 3 次共 1440 次。

比较“全量工具”和“普通排序选前20个工具”，各跑一次：

```bash
python3 tests/rag_effectiveness/run.py --live --selection similarity --k 20 --repeats 1
```

`--live` 会同时运行两组；`--retrieval-only` 只测检索，不调用对话模型。
和此前结果对比时，保持对话模型、Embedding、目录和问题不变。

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

k 固定，默认 10，可用 `--k` 修改：只按排名选前 k 个有效工具，不按相似度分数过滤，
包括负分候选。目录工具数不超过 k 时退化为全量（返回全部工具）。
三个目录都覆盖全部 80 题，因此各档结果可以直接横向比较。

## 5. 工具数量如何增加

- **真实工具 4 个**：单独启动 MCP Server，通过项目 `MCPAgentIntegration` 发现
  天气插件的 `get_weather`，以及日志插件的 `tail_log`、`analyze_log`、`summarize_errors`。
- **50 / 100 / 300 工具目录**：保留这 4 个工具的真实定义，从 304 个可区分的模拟定义里按前缀取。
  其中 12 个是贴着真实工具造的近似能力（历史天气、空气质量、日志正则、全文读取、计数等），
  34 个是热门 MCP 服务形态的空壳工具（GitHub、Slack、Notion、文件系统、Postgres、
  浏览器、Web 搜索、Sentry、Jira、Linear、Gmail、Google Calendar、Drive、Memory），
  其余 258 个是 86 个业务域 × list/get/create 的批量生成项。
  名称和功能说明各不相同，参数按能力定义，不是给同一个工具加编号。
- 50 工具目录 = 4 真实 + 12 近似 + 34 空壳；100 再加 50 个批量生成项；300 再加 200 个。
- 每个目录中，全量组与 RAG 组使用同一批定义；50、100、300 是同一列表的前缀关系。
- 模拟工具只有元数据，**不是新增的真实功能插件**。本评测不执行它们，也不执行天气等业务API，
  以免外部天气服务或文件状态干扰“检索与工具选择”的评价。
- 部分测试题的预期工具就是这些空壳工具（跨应用场景）。评分只比对 `tool_calls` 的名称、
  参数类型和关键参数值，不执行工具，因此空壳不会让这类题失效；它们用来测跨域检索与
  细粒度区分（例如 `count_log_lines` 与 `analyze_log`、`fs_read_file` 与 `read_entire_log`）。
- 检索直接复用项目的 `ToolRetriever / EmbeddingService / VectorIndex / EmbeddingCache`，
  候选定义复用 `MCPAgentIntegration::toFunctionCallingFormat`。Python 只负责目录、调度、评分和展示。

数据集覆盖中文改写、英文、多工具需求、无需工具和近似功能边界，
共 80 题，三类比例约 5:3:2：40 单工具、24 多工具、16 无需工具。
当前 24 道多工具题每题恰好需要 2 个不同工具，暂未覆盖 3 个及以上工具的需求。
参数提供足够信息，例如天气问题直接给出经纬度；评分接受等价坐标字符串及中英文城市名。
标签和生成规则见 `dataset.py`，结果保存实际使用的所有工具定义。

这些样本是人工构造的有限测试集，扩展目录是模拟规模。
结论只能描述这组测试中的表现，不代表生产准确率。无需工具的问题也会收到候选工具，
所以无需工具的 16 道问题用于观察模型会不会错误调用。

## 6. 可选参数

```bash
python3 tests/rag_effectiveness/run.py --live --repeats 1 --k 20
```

- `--model`：默认 `qwen-plus`。
- `--chat-key-env`：对话模型密钥所在环境变量，默认 `DASHSCOPE_API_KEY`；换 DeepSeek 时指向 `DEEPSEEK_API_KEY`。
- `--embedding-model`、`--dimension`：默认 `text-embedding-v2`、1536，修改时确保实际向量维度一致。
- `--k`：RAG 候选数，默认 10；目录工具数不超过 k 时退化为全量。
- `--sizes 50,100,300`：评测的目录规模，默认三档。
- `--chat-url`、`--embedding-url`：自定义兼容接口；默认使用阿里百炼中国区。
- `--output`：结果根目录；每次仍创建独立子目录。
- `--jobs`：构建并行数，默认2。

目录规模不能小于真实工具数（4），也不能超过工具池大小（308）。

## 7. MMR 与漏工具原因

先只调用 Embedding，比较同一批查询在普通排序和 MMR 下有没有把工具给齐：

```bash
python3 tests/rag_effectiveness/run.py --retrieval-only --selection mmr --k 10 --mmr-fetch-k 30 --mmr-lambda 0.7
```

默认仍为 `--selection similarity`。MMR 复用一次问题向量化，从最多30个候选中逐次选出最多10个；
不增加拆分模型请求。权重1等价于普通排序，0.7是实验起点，不代表已验证的最佳值。

报告新增普通排序与 MMR 的给齐题数对照、多工具题给齐数，以及逐题漏工具原因表：
在候选池之外、进入候选池但被MMR排除、普通排序被K截断，或没有有效工具向量。
表中展示原始排名和相似度；按遗漏工具次数统计原因，按题统计完整命中率。

仅检索模式不需要对话模型密钥，不生成对话正确率或输入token结论。要测最终调用效果，
使用原来的 `--live` 命令并加 `--selection mmr`，其余模型和接口参数保持不变。
普通排序与 MMR 的检索对照复用查询向量，不额外消耗 Embedding 请求。

完整说明、C++配置与边界见 [MMR工具筛选说明](../../docs/rag-mmr.md)。
