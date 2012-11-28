# -*- coding: utf-8 -*-

import os
import time
import base64
from os.path import join, abspath, dirname

from tornado.web import Application
from tornado.web import RequestHandler
from tornado.web import StaticFileHandler
from tornado.httpserver import HTTPServer
from tornado.ioloop import IOLoop
from multiprocessing import Process

LOCAL_FILE = lambda *path: join(abspath(dirname(__file__)), *path)


class SimpleHandler(RequestHandler):
    def get(self, name):
        self.render('{}.html'.format(name))


class AuthHandler(RequestHandler):
    def ask_for_pw(self):
        self.set_status(401)
        self.set_header('WWW-Authenticate', 'Basic realm=Restricted')
        self._transforms = []
        self.finish()

    def require_auth(self):
        auth_header = self.request.headers.get('Authorization')
        if auth_header:
            auth_decoded = base64.decodestring(auth_header[6:])
            return auth_decoded.split(':', 2)

        self.ask_for_pw()
        return None, None

    def get(self, name):
        user, password = self.require_auth()

        passed = True
        if user != 'lincoln':
            passed = False

        if password != 'gabriel':
            passed = False

        if not passed:
            self.ask_for_pw()

        self.render('{}.html'.format(name))


class Server(object):
    is_running = False

    def __init__(self, port):
        self.port = int(port)
        self.process = None
        self.options = {
            'template_path': LOCAL_FILE('templates'),
            'cookie_secret': 'cookie-monster',
        }

    @classmethod
    def get_handlers(cls, options):
        return Application([
            (r"/(\w+)", SimpleHandler),
            (r"/auth/(\w+)", AuthHandler),
            (r"/media/(.*)", StaticFileHandler, {"path": LOCAL_FILE('media')}),
        ], **options)

    def start(self):
        def go(app, port, data={}):
            http = HTTPServer(app)
            http.listen(int(port))
            IOLoop.instance().start()

        app = self.get_handlers(self.options)

        data = {}
        args = (app, self.port, data)
        self.process = Process(target=go, args=args)
        self.process.start()

        time.sleep(0.4)

    def stop(self):
        try:
            os.kill(self.process.pid, 9)
        except OSError:
            self.process.terminate()
        finally:
            self.is_running = False


if __name__ == '__main__':
    server = Server(4000)
    server.start()
    started = time.time()
    print "The server is running on port 4000...\n"
    while True:
        try:
            time.sleep(1)
            print "\033[Auptime: %d" % (time.time() - started)
        except KeyboardInterrupt:
            print "\rControl-C was pressed\n"
            raise SystemExit(1)
