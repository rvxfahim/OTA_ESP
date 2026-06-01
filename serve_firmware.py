import http.server
import socketserver
import os

PORT = 8000
DIRECTORY = os.path.join(os.getcwd(), 'test')

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIRECTORY, **kwargs)

with socketserver.TCPServer(("", PORT), Handler) as httpd:
    print(f"Serving at port {PORT} form directory {DIRECTORY}")
    print(f"Test firmware should be at: http://<your-ip>:{PORT}/firmware.bin")
    httpd.serve_forever()
