# RAG 工具选择对照

在项目根目录运行，适用于 Linux / WSL、Python 3.10+，无需额外 Python 包。
脚本自动构建真实 C++ 检索器、MCP 服务端和天气/日志插件。

## 离线自检

```bash
python3 tests/rag_effectiveness/run.py --self-test
```

自检使用本机模拟接口，不需要密钥，不产生真实效果结论。Ubuntu 构建依赖：

```bash
sudo apt-get install -y build-essential cmake pkg-config libcurl4-openssl-dev libjsoncpp-dev nlohmann-json3-dev
```

## 运行真实对照

配置好 `DASHSCOPE_API_KEY` 后运行：

```bash
# 全量工具 vs 普通向量RAG
python3 tests/rag_effectiveness/run.py --live --selection similarity --k 20 --repeats 1

# 全量工具 vs 向量＋关键词倒排RAG
python3 tests/rag_effectiveness/run.py --live --selection hybrid --k 20 --repeats 1
```

每个命令分别跑80题、50/100/300三档、全量和RAG两组，重复1次共480次对话请求。
`--quick` 只跑12题和50工具目录，用于验证接口。
默认模型 `qwen-plus`，Embedding `text-embedding-v2`，K=20。
换模型时保留自己的 `--model`、`--chat-url`、`--chat-key-env` 参数；
Embedding 始终读取 `DASHSCOPE_API_KEY`。密钥不写入命令参数、报告或源代码。
缺少密钥退出77，不能当作测试通过。

## 报告只看两项

`tests/rag_effectiveness/results/live-日期-编号/report.md` 只保留一张主表：

| 工具总数 | 不使用RAG：命中率 | 使用RAG：命中率 | 不使用RAG：平均输入token | 使用RAG：平均输入token |
|---:|---:|---:|---:|---:|
| 50 | 实测 | 实测 | 实测 | 实测 |
| 100 | 实测 | 实测 | 实测 | 实测 |
| 300 | 实测 | 实测 | 实测 | 实测 |

命中率只看模型是否选齐所需工具，不漏选、不多选、不重复；参数错误不扣分，
包括参数 JSON 无法解析。候选集合之外的调用不算对。
无需工具的题没有工具调用即算对。括号显示选对题数/有效题数。
平均输入量使用接口真实 `usage.prompt_tokens`，不按字符估算。
接口失败单独显示次数，不作为工具选错或0 token；存在失败时结果标为不完整。

完整逐题记录、遗漏原因、两路排名及接口错误保存在
`build/rag_effectiveness/logs/<运行编号>/evaluation_details.json`。
结果目录只留一个精简中文报告，默认被 Git 忽略。

## 只测检索

```bash
python3 tests/rag_effectiveness/run.py --retrieval-only --selection hybrid --k 20
```

只调用真实 Embedding，不调用对话模型。报告比较普通向量与混合检索的工具找齐率，
不能把它当作模型选择命中率或输入 token 结果。普通向量对照复用相同查询向量，
不增加 Embedding 请求。

混合检索每路默认取前50个，按 RRF 融合后选20个；
使用 `--hybrid-fetch-k` 和 `--rrf-k` 可调整，默认融合常数60。
默认仍是普通向量；`--selection mmr` 保留为独立实验方式。

## 题目与工具

- 80题：40道单工具、24道双工具、16道无需工具。多工具题当前最少和最多都是2个工具。
- 4个真实工具通过项目 MCP 发现：`get_weather`、`tail_log`、`analyze_log`、`summarize_errors`。
- 50/100/300目录保留真实定义并加入模拟定义，三个目录是同一列表的前缀。
- 两组使用相同问题、定义和模型；RAG最多提供K个工具。目录不超过K时返回全量。
- 本评测不执行工具业务，只检查模型返回的工具调用；一次模型响应就评分，未模拟后续执行轮次。
- 模拟目录和有限样本下的结果不代表生产任务成功率。

更多说明见 [混合检索](../../docs/rag-hybrid.md) 和 [MMR](../../docs/rag-mmr.md)。
