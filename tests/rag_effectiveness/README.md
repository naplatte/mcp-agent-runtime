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

## MMR 专项：什么时候有帮助，什么时候会误伤

新增独立的18题，不改动原80题，也不合并统计：

| 测试情况 | 题数 | 观察什么 |
|---|---:|---|
| 跨领域多需求 | 6 | 天气、邮件、日志等相似工具较多时，能否留住其他领域的工具；包含2个和3个需求 |
| 单工具对照 | 6 | 只需一个工具时，额外追求差异是否有帮助 |
| 相似工具都需要 | 6 | 如日志原文＋日志摘要，去重复是否会误删必需工具 |

专项目录优先保留全部目标工具的原始定义，再增加天气单项指标、邮箱文件夹操作、
按级别读取日志等相近能力，最后补入普通干扰工具。50/100/300目录仍是同一列表的前缀。
没有修改Embedding分数、预设赢家或把答案提供给检索器。

```bash
python3 tests/rag_effectiveness/run.py --retrieval-only --suite mmr-stress --selection mmr --k 10 --mmr-fetch-k 50 --mmr-lambda 0.7
```

只需要 `DASHSCOPE_API_KEY`，不调用对话模型。可保持其他配置不变，分别用 `--k 5`、
`--k 10`、`--k 20` 观察名额紧张程度的影响；每次两组的K相同。
普通向量与MMR复用同一查询向量。报告按情况显示找齐题数、MMR补回题数、MMR弄丢题数，
逐题ID和完整排名留在 `evaluation_details.json`。即使总分不变，也能看出是否有得有失。
专项仅支持 `--retrieval-only --selection mmr`，不支持 `--quick`。
它用于研究算法边界，真实收益需要运行后判断，不能代替原80题的日常效果对照。

## 题目与工具

- 80题：40道单工具、24道双工具、16道无需工具。多工具题当前最少和最多都是2个工具。
- 4个真实工具通过项目 MCP 发现：`get_weather`、`tail_log`、`analyze_log`、`summarize_errors`。
- 50/100/300目录保留真实定义并加入模拟定义，三个目录是同一列表的前缀。
- 两组使用相同问题、定义和模型；RAG最多提供K个工具。目录不超过K时返回全量。
- 本评测不执行工具业务，只检查模型返回的工具调用；一次模型响应就评分，未模拟后续执行轮次。
- 模拟目录和有限样本下的结果不代表生产任务成功率。

更多说明见 [混合检索](../../docs/rag-hybrid.md) 和 [MMR](../../docs/rag-mmr.md)。
