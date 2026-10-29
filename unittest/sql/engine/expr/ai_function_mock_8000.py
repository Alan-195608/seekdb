#!/usr/bin/env python3
"""Deterministic AI mock that also serves the organizer's mock path layout.

The official ``ai_function_enhancement`` test points its endpoints at
``http://0.0.0.0:8000/compatible-mode/v1/...``.  This server answers both that
layout and the plain ``/v1/...`` layout so the same oracle can back either
endpoint configuration.  It performs no inference and needs no credentials.
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def _embedding_vectors(inputs, dimension):
    return [[float(sum(text.encode("utf-8")) % 97 + i) for i in range(dimension)]
            for text in inputs]


# The organizer's mock answers an embedding request without an explicit
# "dimensions" field with a 768 element vector, so the mock used to run the
# official mysqltest has to use the same default.
DEFAULT_EMBEDDING_DIMENSION = 768


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(length)) if length else {}
            path = self.path
            if path.endswith("/api/generate"):
                # Ollama native completion protocol.
                assert body.get("stream") is False
                result = {"response": "reply:" + body["prompt"], "done": True}
            elif path.endswith("/chat/completions"):
                text = body["messages"][-1]["content"]
                result = {"choices": [{"message": {"role": "assistant",
                                                   "content": "reply:" + text}}]}
            elif path.endswith("/embeddings") or path.endswith("/api/embed"):
                inputs = body["input"]
                if isinstance(inputs, str):
                    inputs = [inputs]
                dimension = body.get("dimensions", DEFAULT_EMBEDDING_DIMENSION)
                vectors = _embedding_vectors(inputs, dimension)
                if path.endswith("/api/embed"):
                    result = {"embeddings": vectors}
                else:
                    # The response order is reversed on purpose: an implementation
                    # that trusts the response order instead of the index shows up
                    # as mismatched rows.
                    result = {"data": list(reversed(
                        [{"index": i, "embedding": vector}
                         for i, vector in enumerate(vectors)]))}
            elif path.endswith("/rerank"):
                result = {"results": list(reversed(
                    [{"index": i,
                      "relevance_score": 1.0 if body["query"] in text else 0.0}
                     for i, text in enumerate(body["documents"])]))}
            else:
                self.send_error(404)
                return
            encoded = json.dumps(result, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)
        except (AssertionError, KeyError, ValueError, TypeError) as error:
            self.send_error(400, str(error))


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()
