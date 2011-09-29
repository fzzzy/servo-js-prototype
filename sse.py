
import eventlet
from eventlet import wsgi
import random

def app(environ, start_response):
    if environ['PATH_INFO'] == '/':
        start_response('200 OK', [('Content-type', 'text/html')])
        yield file('sse.html').read()
        return
    start_response('200 OK', [('Content-type', 'text/event-stream')])
    while True:
        choice = random.choice(["data: foo\n\n", "data: bar\n\n", "data: baz\nfrotz\n\n"])
        print choice
        yield choice
        eventlet.sleep(random.choice([0.1, 0.2, 0.5, 0.7, 1, 1.5, 2, 3]))

wsgi.server(eventlet.listen(('localhost', 6999)), app)
