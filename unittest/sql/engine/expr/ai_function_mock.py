#!/usr/bin/env python3
"""Protocol oracle for SQL integration tests; no model credentials or inference."""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            inputs = body.get('input', [])
            print(json.dumps({'path': self.path, 'input_count': len(inputs) if isinstance(inputs, list) else 1}), flush=True)
            if self.path == '/api/generate':
                assert body['stream'] is False
                result = {'response': 'reply:' + body['prompt'], 'done': True}
            elif self.path.endswith('/chat/completions'):
                text = body['messages'][-1]['content']
                result = {'choices': [{'message': {'role': 'assistant', 'content': 'reply:' + text}}]}
            elif self.path.endswith('/embeddings') or self.path == '/api/embed':
                inputs = body['input']
                if isinstance(inputs, str):
                    inputs = [inputs]
                dimension = body.get('dimensions', 4)
                vectors = [[float(sum(text.encode('utf-8')) % 97 + i) for i in range(dimension)] for text in inputs]
                if self.path == '/api/embed':
                    result = {'embeddings': vectors}
                else:
                    result = {'data': list(reversed([{'index': i, 'embedding': vector} for i, vector in enumerate(vectors)]))}
            elif self.path.endswith('/rerank'):
                # Deliberately unordered responses and tied scores expose incorrect
                # reliance on response order or a provider's tie-breaking behavior.
                result = {'results': list(reversed([{'index': i, 'relevance_score': 1.0 if body['query'] in text else 0.0} for i, text in enumerate(body['documents'])]))}
            else:
                self.send_error(404)
                return
            encoded = json.dumps(result, ensure_ascii=False).encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)
        except (AssertionError, KeyError, ValueError, TypeError) as error:
            self.send_error(400, str(error))

ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), Handler).serve_forever()
