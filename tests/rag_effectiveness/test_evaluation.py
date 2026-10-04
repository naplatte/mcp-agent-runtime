"""Offline, meaningful regression checks. All HTTP is on 127.0.0.1."""
import contextlib
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
from evaluation import aggregate, coverage, score
from run import Bridge, evaluate


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
    def test_cases_and_catalogs_have_distinct_capabilities(self):
        suite = cases()
        self.assertEqual(len(suite), 30)
        self.assertEqual(sum(c["split"] == "dev" for c in suite), 6)
        self.assertTrue(any(c["kind"] == "multi" and c["split"] == "dev" for c in suite))
        base = [definition(name, name, {}, []) for name in ("get_weather", "tail_log", "analyze_log", "summarize_errors")]
        groups = catalogs(base, [50, 100])
        self.assertEqual([len(g) for g in groups], [4, 50, 100])
        self.assertEqual(groups[2][:50], groups[1])
        self.assertEqual(len({t["name"] for t in groups[-1]}), 100)
        self.assertEqual(len({t["description"] for t in groups[-1]}), 100)

    def test_tool_and_argument_failures_are_scored_separately(self):
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
        self.assertFalse(scored["correct"])
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
        self.assertFalse(score({"content": ""}, no_tool, [])["correct"])

    def test_api_errors_are_not_zero_tokens_or_success(self):
        result = aggregate([{"api_error": "HTTP 401"}])
        self.assertEqual(result["api_errors"], 1)
        self.assertEqual(result["valid"], 0)
        self.assertIsNone(result["accuracy"])
        self.assertIsNone(result["prompt_tokens"])


@unittest.skipUnless(os.environ.get("RAG_EVAL_BACKEND"), "Build the C++ backend using run.py --self-test")
class BackendTests(unittest.TestCase):
    def test_actual_cpp_discovery_retrieval_cache_and_error_boundary(self):
        with tempfile.TemporaryDirectory() as directory, local_api() as (api, url):
            bridge = Bridge(os.environ["RAG_EVAL_BACKEND"], directory)
            try:
                tools = bridge.request({"op": "discover", "server": os.environ["RAG_EVAL_SERVER"],
                                        "plugins": os.environ["RAG_EVAL_PLUGINS"], "logs": directory})["tools"]
                self.assertEqual({t["name"] for t in tools}, {"get_weather", "tail_log", "analyze_log", "summarize_errors"})
                response = bridge.request({"op": "index", "tools": tools, "fixture_key": "offline-fixture",
                                           "dimension": 8, "threshold": 0.3, "embedding_url": url + "/embedding",
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
                with self.assertRaisesRegex(RuntimeError, "smaller"):
                    bridge.request({"op": "retrieve", "query": "北京天气", "k": 4})
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
        with tempfile.TemporaryDirectory() as directory, local_api() as (_, url), patch.dict(os.environ, {"DASHSCOPE_API_KEY": "offline-fixture"}):
            args = types.SimpleNamespace(quick=False, repeats=2, model="fixture", embedding_model="fixture",
                                         dimension=8, threshold=0.3, sizes=[50, 100], k_values=[1, 3, 5],
                                         chat_url=url + "/chat", embedding_url=url + "/embedding", timeout=3, fixture=True)
            with contextlib.redirect_stdout(io.StringIO()):
                code = evaluate(args, Path(os.environ["RAG_EVAL_BACKEND"]), Path(os.environ["RAG_EVAL_SERVER"]),
                                Path(os.environ["RAG_EVAL_PLUGINS"]), Path(directory))
            self.assertEqual(code, 0)
            data = json.loads((Path(directory) / "summary.json").read_text())
            self.assertEqual(data["evidence"], "fixture")
            self.assertEqual(len(data["records"]), 288)
            self.assertEqual([s["size"] for s in data["stages"]], [4, 50, 100])
            self.assertTrue(all(s["query_misses"] == 24 and s["query_hits"] == 0 for s in data["stages"]))
            self.assertTrue(all(r["query_cache_misses"] == 0 for r in data["records"] if r["repeat"] == 2))
            self.assertIn("不能作为真实RAG效果结论", (Path(directory) / "report.md").read_text())
            self.assertEqual(len((Path(directory) / "details.jsonl").read_text().splitlines()), 288)


if __name__ == "__main__":
    unittest.main()
