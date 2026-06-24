import json
import sys
import urllib.request


class MCPClient:
    def __init__(self, url):
        self.url = url
        self.session_id = None

    def request(self, method, params=None, request_id="1"):
        body = {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
        }
        if params is not None:
            body["params"] = params

        data = json.dumps(body).encode("utf-8")
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
            "MCP-Protocol-Version": "2025-11-25",
        }
        if self.session_id:
            headers["MCP-Session-Id"] = self.session_id

        req = urllib.request.Request(self.url, data=data, headers=headers, method="POST")
        with urllib.request.urlopen(req, timeout=30) as resp:
            session_id = resp.headers.get("MCP-Session-Id")
            if session_id:
                self.session_id = session_id
            return json.loads(resp.read().decode("utf-8"))


def load_url(config_path):
    with open(config_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    servers = data.get("mcpServers", {})
    for server in servers.values():
        return server["url"]
    raise RuntimeError("No MCP server URL found")


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 test-client-streamable-http.py <configuration.json>")
        sys.exit(1)

    client = MCPClient(load_url(sys.argv[1]))

    init_params = {
        "protocolVersion": "2025-11-25",
        "capabilities": {},
        "clientInfo": {"name": "streamable-http-test", "version": "1.0.0"},
    }
    print("initialize:", client.request("initialize", init_params, "1"))

    tools = client.request("tools/list", request_id="2")
    print("tools/list:", tools)

    result = client.request(
        "tools/call",
        {"name": "add", "arguments": {"a": 1, "b": 2}},
        "3",
    )
    print("tools/call:", result)


if __name__ == "__main__":
    main()
