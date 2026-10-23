"""Offline, meaningful regression checks. All HTTP is on 127.0.0.1."""
import contextlib
from collections import Counter
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import types
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

from dataset import cases, catalogs, definition, text
from evaluation import aggregate, coverage, report, score
from run import Bridge, evaluate, retrieval_misses
import run
import mmr_dataset


def fixture_vector(value):
    tokens = [("get_weather", "天气"), ("tail_log", "最后"),
              ("analyze_log", "timeout"), ("summarize_errors", "摘要")]
    vector = [0.0] * 8
    for i, words in enumerate(tokens):
        if any(word in value for word in words):
            vector[i] = 1.0
    if not any(vector):
        vector[4] = 1.0
    return vector


class Fixture(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append((self.path, request))
        if self.path == "/embedding":
            if self.server.mode == "error":
                result = {"code": "fixture_error", "message": "fixture refused embedding"}
            else:
                result = {"output": {"embeddings": [
                    {"embedding": [] if self.server.mode == "empty" else fixture_vector(value), "text_index": i}
                    for i, value in enumerate(request["input"]["texts"])]}}
        elif self.path == "/chat":
            question = request["messages"][-1]["content"]
            case = next(c for c in cases() if c["question"] == question)
            calls = [{"id": f"call_{i}", "type": "function", "function": {
                "name": call["name"], "arguments": json.dumps(call["arguments"], ensure_ascii=False)}}
                     for i, call in enumerate(case["expected_calls"])]
            message = {"role": "assistant", "content": None if calls else "这是不需要工具的直接回答。", "tool_calls": calls}
            # Unit fixture values only; never presented as real token measurements.
            result = {"choices": [{"message": message}], "usage": {
                "prompt_tokens": 10 + len(request["tools"]) * 20, "completion_tokens": 5}}
        else:
            self.send_error(404)
            return
        payload = json.dumps(result).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


@contextlib.contextmanager
def local_api():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
    server.mode = "ok"
    server.requests = []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server, f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


class ScoringTests(unittest.TestCase):
    def test_mmr_stress_cases_keep_original_suite_and_labels_separate(self):
        suite = mmr_dataset.cases()
        self.assertEqual(len(suite), 18)
        self.assertEqual(Counter(c['scenario'] for c in suite),
                         {scenario: 6 for scenario in mmr_dataset.SCENARIOS})
        self.assertEqual({len(c['expected_calls']) for c in suite}, {1, 2, 3})
        self.assertTrue({c['id'] for c in suite}.isdisjoint(c['id'] for c in cases()))
        base = [definition(name, name, {}, []) for name in
                ('get_weather', 'tail_log', 'analyze_log', 'summarize_errors')]
        groups = mmr_dataset.catalogs(base, [50, 100, 300])
        self.assertEqual([len(group) for group in groups], [50, 100, 300])
        self.assertEqual(groups[1][:50], groups[0])
        self.assertEqual(groups[2][:100], groups[1])
        required = {call['name'] for case in suite for call in case['expected_calls']}
        for group in groups:
            names = [tool['name'] for tool in group]
            self.assertEqual(len(names), len(set(names)))
            self.assertTrue(required <= set(names))
        self.assertEqual(len(cases()), 80)
        self.assertGreater(sum(t['name'].startswith('weather_') for t in groups[1]),
                           sum(t['name'].startswith('weather_') for t in groups[0]))

    def test_mmr_stress_report_counts_both_gains_and_regressions(self):
        suite = mmr_dataset.cases()
        def response(case, hit):
            names = [call['name'] for call in case['expected_calls']] if hit else []
            return {'tools': [{'name': name} for name in names]}
        before = {case['id']: response(case, case['id'] != 'crowded-1') for case in suite}
        after = {case['id']: response(case, case['id'] != 'similar-1') for case in suite}
        comparison = run.scenario_comparison(suite, before, after)
        self.assertEqual(comparison[0]['gained_ids'], ['crowded-1'])
        self.assertEqual(comparison[2]['lost_ids'], ['similar-1'])
        stage = {'size': 50, 'k': 5, 'retrieval': run.retrieval_metrics(suite, after),
                 'baseline_retrieval': run.retrieval_metrics(suite, before), 'scenario_comparison': comparison}
        data = {'status': '完成：仅检索', 'cases': suite, 'config': {
            'suite': 'mmr-stress', 'selection': 'mmr', 'retrieval_only': True, 'model': 'fixture',
            'embedding_model': 'fixture', 'repeats': 1, 'mmr_fetch_k': 30, 'mmr_lambda': 0.7},
            'stages': [stage]}
        with tempfile.TemporaryDirectory() as directory:
            report(data, directory)
            rendered = (Path(directory) / 'report.md').read_text(encoding='utf-8')
        self.assertIn('不与原80题混合统计', rendered)
        self.assertIn('MMR补回 | MMR弄丢', rendered)
        self.assertIn('| 50 | 跨领域多需求 | 5/6 | 6/6 | 1 | 0 |', rendered)
        self.assertIn('| 50 | 相似工具都需要 | 6/6 | 5/6 | 0 | 1 |', rendered)

    def test_miss_diagnostics_distinguish_pool_and_selection(self):
        case = {"id": "fixture", "kind": "single", "question": "fixture",
                "expected_calls": [{"name": "target", "arguments": {}}]}
        entry = {"name": "target", "rank": 31, "score": -0.2, "in_pool": False}
        response = {"tools": [], "ranking": [entry]}
        def reason(selection="mmr"):
            return retrieval_misses([case], {"fixture": response}, selection, 10, 30)[0]
        self.assertEqual(reason()["reason_code"], "pool")
        self.assertEqual(reason()["rank"], 31)
        entry.update(rank=12, in_pool=True)
        self.assertEqual(reason()["reason_code"], "mmr")
        self.assertEqual(reason("similarity")["reason_code"], "top_k")
        response["tools"] = [{"name": "target"}]
        self.assertEqual(retrieval_misses([case], {"fixture": response}, "mmr", 10, 30), [])
        response.update(tools=[], ranking=[])
        self.assertEqual(reason()["reason_code"], "missing_vector")

    def test_cases_and_catalogs_have_distinct_capabilities(self):
        suite = cases()
        self.assertEqual(len(suite), 80)
        kinds = Counter(c["kind"] for c in suite)
        self.assertEqual((kinds["single"], kinds["multi"], kinds["no_tool"]), (40, 24, 16))
        base = [definition(name, name, {}, []) for name in ("get_weather", "tail_log", "analyze_log", "summarize_errors")]
        groups = catalogs(base, [50, 100, 300])
        self.assertEqual([len(g) for g in groups], [50, 100, 300])
        self.assertEqual(groups[1][:50], groups[0])
        self.assertEqual(groups[2][:100], groups[1])
        self.assertEqual(len({t["name"] for t in groups[-1]}), 300)
        self.assertEqual(len({t["description"] for t in groups[-1]}), 300)

    def test_only_tool_names_are_scored_even_with_bad_arguments(self):
        tool = definition("get_weather", "weather", {"city": text("City"), "latitude": text("Latitude"),
                                                   "longitude": text("Longitude")}, ["city", "latitude", "longitude"])
        case = cases()[0]

        def message(arguments, name="get_weather"):
            return {"tool_calls": [{"function": {"name": name, "arguments": json.dumps(arguments)}}]}

        good = {"city": "Beijing", "latitude": "39.900", "longitude": "116.400"}
        self.assertTrue(score(message(good), case, [tool])["correct"])
        bad = dict(good, latitude=39.9)
        scored = score(message(bad), case, [tool])
        self.assertTrue(scored["selection_correct"])
        self.assertTrue(scored["correct"])
        malformed = message(good)
        malformed['tool_calls'][0]['function']['arguments'] = '{invalid-json'
        self.assertTrue(score(malformed, case, [tool])["correct"])
        del malformed['tool_calls'][0]['function']['arguments']
        self.assertTrue(score(malformed, case, [tool])["correct"])
        self.assertFalse(score(message(good), case, [])["correct"])
        self.assertFalse(score(message(good, "air_quality"), case, [tool])["correct"])
        duplicate = message(good)
        duplicate["tool_calls"] *= 2
        self.assertFalse(score(duplicate, case, [tool])["selection_correct"])

    def test_multi_tool_coverage_and_no_tool(self):
        case = next(c for c in cases() if c["kind"] == "multi")
        self.assertEqual(coverage(case, ["get_weather"]), (0.5, False))
        self.assertEqual(coverage(case, ["get_weather", "tail_log"]), (1.0, True))
        no_tool = next(c for c in cases() if c["kind"] == "no_tool")
        self.assertTrue(score({"content": "你好"}, no_tool, [])["correct"])
        self.assertTrue(score({"content": ""}, no_tool, [])["correct"])
        self.assertFalse(score({"tool_calls": [{}]}, no_tool, [])["correct"])

    def test_api_errors_are_not_zero_tokens_or_success(self):
        result = aggregate([{"api_error": "HTTP 401"}])
        self.assertEqual(result["api_errors"], 1)
        self.assertEqual(result["valid"], 0)
        self.assertIsNone(result["accuracy"])
        self.assertIsNone(result["prompt_tokens"])

    def test_compact_report_discloses_api_failures_without_parameter_metrics(self):
        valid = {"selection_correct": True, "prompt_tokens": 321, "retrieval_hit": True}
        summary = aggregate([valid, {"api_error": "HTTP 500"}])
        data = {"status": "不完整：存在API失败", "config": {"model": "fixture", "embedding_model": "fixture",
                "repeats": 1, "selection": "hybrid"}, "cases": cases(),
                "stages": [{"size": 50, "k": 20, "summary": {"all": summary, "rag": summary}}]}
        with tempfile.TemporaryDirectory() as directory:
            report(data, directory)
            rendered = (Path(directory) / 'report.md').read_text(encoding='utf-8')
        self.assertIn('100.0%（1/1）', rendered)
        self.assertIn('有效1/2，接口失败1次', rendered)
        self.assertIn('321', rendered)
        self.assertNotIn('命中题正确率', rendered)
        self.assertNotIn('参数错1题', rendered)


@unittest.skipUnless(os.environ.get("RAG_EVAL_BACKEND"), "Build the C++ backend using run.py --self-test")
class BackendTests(unittest.TestCase):
    def test_mmr_stress_driver_uses_real_cpp_and_cached_vector_for_both_arms(self):
        with tempfile.TemporaryDirectory() as directory, local_api() as (api, url), \
                patch.dict(os.environ, {'DASHSCOPE_API_KEY': 'offline-fixture'}):
            args = types.SimpleNamespace(suite='mmr-stress', quick=False, repeats=1, model='fixture',
                                         embedding_model='fixture', dimension=8, sizes=[50, 100], k=5,
                                         selection='mmr', mmr_fetch_k=30, mmr_lambda=0.7,
                                         retrieval_only=True, fixture=True, chat_url=url + '/chat',
                                         embedding_url=url + '/embedding', timeout=3)
            with contextlib.redirect_stdout(io.StringIO()):
                code = evaluate(args, Path(os.environ['RAG_EVAL_BACKEND']), Path(os.environ['RAG_EVAL_SERVER']),
                                Path(os.environ['RAG_EVAL_PLUGINS']), Path(directory), Path(directory) / 'logs')
            self.assertEqual(code, 0)
            data = run.LAST_RUN
            self.assertEqual(len(data['cases']), 18)
            self.assertFalse(any(path == '/chat' for path, _ in api.requests))
            self.assertEqual(data['records'], [])
            for stage in data['stages']:
                self.assertEqual(stage['cases'], 18)
                self.assertEqual(stage['query_misses'], 18)
                self.assertEqual(len(stage['scenario_comparison']), 3)
                for query in stage['queries'].values():
                    self.assertEqual(len(query['tools']), 5)
            requests = [request for path, request in api.requests if path == '/embedding']
            # One query embedding per question and catalog, including the paired baseline.
            questions = {case['question'] for case in data['cases']}
            self.assertEqual(sum(value in questions for request in requests
                                 for value in request['input']['texts']), 36)
            persisted = json.loads((Path(directory) / 'logs' / 'evaluation_details.json').read_text(encoding='utf-8'))
            self.assertIn('scenario_comparison', persisted['stages'][0])

    def test_hybrid_recovers_keyword_tool_and_rebuilds_after_reload_without_extra_embedding(self):
        with tempfile.TemporaryDirectory() as directory, local_api() as (api, url):
            bridge = Bridge(os.environ["RAG_EVAL_BACKEND"], directory)
            try:
                tools = [definition("a_control", "Inspect host status.", {}, []),
                         definition("b_control", "Read resource configuration.", {}, []),
                         definition("c_control", "List pending tasks.", {}, []),
                         definition("z_packager", "Export tarball archives.", {}, [])]
                bridge.request({"op": "index", "tools": tools, "fixture_key": "offline-fixture",
                                "dimension": 8, "embedding_url": url + "/embedding",
                                "index_path": str(Path(directory) / "index.json")})
                request = {"op": "retrieve", "query": "tarball", "k": 2,
                           "hybrid_fetch_k": 2, "diagnostics": True}
                ordinary = bridge.request(dict(request, selection="similarity"))
                self.assertNotIn("z_packager", [tool["name"] for tool in ordinary['tools']])
                before = len(api.requests)
                hybrid = bridge.request(dict(request, selection="hybrid"))
                self.assertEqual(len(api.requests), before)
                self.assertEqual(hybrid['cache_misses'], 0)
                self.assertEqual(len(hybrid['tools']), 2)
                self.assertIn("z_packager", [tool["name"] for tool in hybrid['tools']])
                target = next(item for item in hybrid['ranking'] if item['name'] == 'z_packager')
                self.assertEqual((target['rank'], target['keyword_rank']), (4, 1))
                self.assertTrue(target['in_pool'])
                with self.assertRaisesRegex(RuntimeError, 'fetch_k'):
                    bridge.request(dict(request, selection='hybrid', hybrid_fetch_k=1))
                bridge.request({"op": "reset_query_cache"})
                reloaded = bridge.request(dict(request, selection='hybrid'))
                self.assertEqual(reloaded['tools'], hybrid['tools'])
                self.assertEqual(len(api.requests), before + 1)
                unmatched = dict(request, query='qqqunmatched', diagnostics=False)
                plain = bridge.request(dict(unmatched, selection='similarity'))
                fused = bridge.request(dict(unmatched, selection='hybrid'))
                self.assertEqual(plain['tools'], fused['tools'])
            finally:
                bridge.close()

    def test_mmr_reuses_query_embedding_and_exposes_unfiltered_ranking(self):
        with tempfile.TemporaryDirectory() as directory, local_api() as (api, url):
            bridge = Bridge(os.environ["RAG_EVAL_BACKEND"], directory)
            try:
                tools = [definition("get_weather", "weather", {}, []),
                         definition("tail_log", "log", {}, []),
                         definition("other", "other", {}, [])]
                bridge.request({"op": "index", "tools": tools, "fixture_key": "offline-fixture",
                                "dimension": 8, "embedding_url": url + "/embedding",
                                "index_path": str(Path(directory) / "index.json")})
                before = len(api.requests)
                query = {"op": "retrieve", "query": "北京天气", "k": 2, "selection": "mmr",
                         "mmr_fetch_k": 3, "mmr_lambda": 0.7, "diagnostics": True}
                mmr = bridge.request(query)
                self.assertEqual(len(api.requests), before + 1)
                self.assertEqual(mmr["cache_misses"], 1)
                self.assertEqual(len(mmr["ranking"]), 3)
                target = next(r for r in mmr["ranking"] if r["name"] == "tail_log")
                self.assertEqual(target["score"], 0.0)
                self.assertTrue(target["in_pool"])
                self.assertFalse(target["selected"])
                self.assertEqual(len(mmr["tools"]), 2)
                ordinary = bridge.request(dict(query, selection="similarity"))
                self.assertEqual(ordinary["cache_misses"], 0)
                self.assertEqual(len(api.requests), before + 1)
                self.assertEqual(mmr["ranking"], ordinary["ranking"])
                with self.assertRaisesRegex(RuntimeError, "fetch_k"):
                    bridge.request(dict(query, mmr_fetch_k=1))
                self.assertEqual(len(api.requests), before + 1)
            finally:
                bridge.close()

    def test_retrieval_only_mmr_report_never_calls_chat(self):
        with tempfile.TemporaryDirectory() as directory, local_api() as (api, url), \
                patch.dict(os.environ, {"DASHSCOPE_API_KEY": "offline-fixture"}):
            args = types.SimpleNamespace(quick=True, repeats=1, model="fixture", embedding_model="fixture",
                                         dimension=8, sizes=[50], k=10,
                                         selection="mmr", mmr_fetch_k=30, mmr_lambda=0.7,
                                         retrieval_only=True, fixture=True,
                                         chat_url=url + "/chat", embedding_url=url + "/embedding", timeout=3)
            with contextlib.redirect_stdout(io.StringIO()):
                code = evaluate(args, Path(os.environ["RAG_EVAL_BACKEND"]), Path(os.environ["RAG_EVAL_SERVER"]),
                                Path(os.environ["RAG_EVAL_PLUGINS"]), Path(directory), Path(directory) / "logs")
            self.assertEqual(code, 0)
            self.assertFalse(any(path == "/chat" for path, _ in api.requests))
            data = run.LAST_RUN
            self.assertEqual(data["records"], [])
            stage = data["stages"][0]
            self.assertEqual(stage["query_misses"], len(data["cases"]))
            self.assertIn("baseline_retrieval", stage)
            text_report = (Path(directory) / "report.md").read_text(encoding="utf-8")
            self.assertIn("普通向量：工具找齐", text_report)
            self.assertIn("MMR：工具找齐", text_report)
            self.assertNotIn("相似度阈值", text_report)
            self.assertNotIn("低于阈值", text_report)
            self.assertIn("模型工具选择命中率和输入 token 未测量", text_report)
            self.assertTrue((Path(directory) / 'logs' / 'evaluation_details.json').exists())

    def test_actual_cpp_discovery_retrieval_cache_and_error_boundary(self):
        with tempfile.TemporaryDirectory() as directory, local_api() as (api, url):
            bridge = Bridge(os.environ["RAG_EVAL_BACKEND"], directory)
            try:
                tools = bridge.request({"op": "discover", "server": os.environ["RAG_EVAL_SERVER"],
                                        "plugins": os.environ["RAG_EVAL_PLUGINS"], "logs": directory})["tools"]
                self.assertEqual({t["name"] for t in tools}, {"get_weather", "tail_log", "analyze_log", "summarize_errors"})
                response = bridge.request({"op": "index", "tools": tools, "fixture_key": "offline-fixture",
                                           "dimension": 8, "embedding_url": url + "/embedding",
                                           "index_path": str(Path(directory) / "index.json")})
                self.assertEqual(response["indexed"], 4)
                first = bridge.request({"op": "retrieve", "query": "北京天气", "k": 1})
                self.assertEqual(first["tools"][0]["name"], "get_weather")
                self.assertEqual(first["cache_misses"], 1)
                before = len(api.requests)
                warm = bridge.request({"op": "retrieve", "query": "北京天气", "k": 1})
                self.assertEqual(warm["cache_hits"], 1)
                self.assertEqual(len(api.requests), before)
                bridge.request({"op": "reset_query_cache"})
                cold = bridge.request({"op": "retrieve", "query": "北京天气", "k": 1})
                self.assertEqual(cold["cache_misses"], 1)
                # k 超过目录大小时按全量返回：4 个工具、k=10 仍应拿到 4 个
                full = bridge.request({"op": "retrieve", "query": "北京天气", "k": 10})
                self.assertEqual(len(full["tools"]), 4)
                with self.assertRaisesRegex(RuntimeError, "positive"):
                    bridge.request({"op": "retrieve", "query": "北京天气", "k": 0})
            finally:
                bridge.close()

    def test_empty_and_failed_embeddings_cannot_pass_index_validation(self):
        for mode in ("empty", "error"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory, local_api() as (api, url):
                api.mode = mode
                bridge = Bridge(os.environ["RAG_EVAL_BACKEND"], directory)
                try:
                    tools = [definition("sample", "fixture", {}, [])]
                    with self.assertRaisesRegex(RuntimeError, "embedding|Incomplete"):
                        bridge.request({"op": "index", "tools": tools, "fixture_key": "offline-fixture",
                                        "dimension": 8, "embedding_url": url + "/embedding",
                                        "index_path": str(Path(directory) / "index.json")})
                finally:
                    bridge.close()

    def test_complete_driver_reports_are_explicitly_fixture_only(self):
        self.check_complete_driver("similarity")

    def test_complete_mmr_driver_reports_are_explicitly_fixture_only(self):
        self.check_complete_driver("mmr")

    def test_complete_hybrid_driver_reports_are_explicitly_fixture_only(self):
        self.check_complete_driver("hybrid")

    def check_complete_driver(self, selection):
        with tempfile.TemporaryDirectory() as directory, local_api() as (_, url), patch.dict(os.environ, {"DASHSCOPE_API_KEY": "offline-fixture"}):
            args = types.SimpleNamespace(quick=False, repeats=2, model="fixture", embedding_model="fixture",
                                         dimension=8, sizes=[50, 100, 300], k=10 if selection == "mmr" else 20,
                                         selection=selection, mmr_fetch_k=30, mmr_lambda=0.7,
                                         chat_url=url + "/chat", embedding_url=url + "/embedding", timeout=3, fixture=True)
            with contextlib.redirect_stdout(io.StringIO()):
                code = evaluate(args, Path(os.environ["RAG_EVAL_BACKEND"]), Path(os.environ["RAG_EVAL_SERVER"]),
                                Path(os.environ["RAG_EVAL_PLUGINS"]), Path(directory), Path(directory) / "logs")
            self.assertEqual(code, 0)
            data = run.LAST_RUN
            self.assertEqual(data["evidence"], "fixture")
            self.assertEqual([s["size"] for s in data["stages"]], [50, 100, 300])
            expected = sum(s["test_cases"] * 2 * args.repeats for s in data["stages"])
            self.assertEqual(len(data["records"]), expected)
            for stage in data["stages"]:
                self.assertTrue(all(len(query['tools']) == stage['k'] for query in stage['queries'].values()))
                # k 不小于目录大小时走退化分支，不产生查询向量
                expected_misses = stage["test_cases"] if stage["k"] < stage["size"] else 0
                self.assertEqual(stage["query_misses"], expected_misses)
                self.assertEqual(stage["query_hits"], 0)
            self.assertTrue(all(r["query_cache_misses"] == 0 for r in data["records"] if r["repeat"] == 2))
            # 结果目录里只留一个中文报告
            self.assertEqual([p.name for p in Path(directory).iterdir() if p.is_file()], ["report.md"])
            text_report = (Path(directory) / "report.md").read_text()
            self.assertIn("不能作为真实 RAG 效果结论", text_report)
            self.assertIn("不使用RAG：命中率", text_report)
            self.assertIn("使用RAG：平均输入token", text_report)
            self.assertNotIn("命中题正确率", text_report)
            self.assertNotIn("相似度阈值", text_report)
            self.assertNotIn("低于阈值", text_report)


if __name__ == "__main__":
    unittest.main()
