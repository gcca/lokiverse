import copy
import gc
import inspect
import pickle
import queue
import subprocess
import sys
import textwrap
import threading
import time
import weakref
import unittest
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import lokiverse
from lokiverse import Logger, Message, StreamBuilder, Transport, Value
from wire_reference import decode, values


@contextmanager
def receiver(status=204, release=None):
    requests = queue.Queue()

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            self.connection.settimeout(3)
            body = self.rfile.read(int(self.headers["Content-Length"]))
            requests.put((self.path, self.headers["Content-Type"], body))
            if release is not None and not release.wait(3):
                self.send_error(500)
                return
            self.send_response(status)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(
        target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True
    )
    thread.start()
    try:
        yield server, requests
    finally:
        if release is not None:
            release.set()
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)


def new_message(entries=None, labels=None):
    message = Message()
    entries = [(1, "hello")] if entries is None else list(entries)
    labels = [("job", "python")] if labels is None else list(labels.items()) if isinstance(labels, dict) else list(labels)
    stream = message.stream().reserve(len(labels), len(entries))
    for name, value in labels:
        stream.label(name, value)
    for timestamp, line in entries:
        stream.entry(timestamp, line)
    stream.commit()
    return message


def http_logger(host="localhost", port=3100, safe=True):
    transport = Transport.make_http(host, port)
    return (Logger.make_safe if safe else Logger.make_unsafe)(transport)


def pushed(message):
    with receiver() as (server, requests):
        assert http_logger("127.0.0.1", server.server_port).push(message)
        path, content_type, body = requests.get(timeout=3)
    assert path == "/loki/api/v1/push"
    assert content_type == "application/x-protobuf"
    assert body == message.encode()
    return decode(body)


def run_child(code):
    return subprocess.run(
        [sys.executable, *(["-S"] if sys.flags.no_site else []), "-X", "dev", "-c", textwrap.dedent(code)],
        cwd=Path(__file__).parent,
        capture_output=True, text=True, timeout=8,
    )


class Sink(Transport):
    def __init__(self, status=204, delay=0):
        self.status = status
        self.delay = delay
        self.requests = []
        self.active = 0
        self.maximum = 0

    def post(self, path, body, content_type):
        self.active += 1
        self.maximum = max(self.maximum, self.active)
        time.sleep(self.delay)
        self.requests.append((path, body, content_type))
        self.active -= 1
        return self.status


class MessageTest(unittest.TestCase):
    def test_multiple_streams_and_encoded_http_body(self):
        message = new_message([(1700000000123456789, "first"), (1700000000123456790, "second")], {"job": "api", "env": "test"})
        message.stream().label("job", "worker").entry(1700000001123456789, "third").commit()
        request = pushed(message)
        self.assertEqual(len(request.streams), 2)
        self.assertEqual(request.streams[0].labels, '{env="test",job="api"}')
        self.assertEqual(values(request.streams[0]), [(1700000000123456789, "first"), (1700000000123456790, "second")])
        self.assertEqual(values(request.streams[1]), [(1700000001123456789, "third")])

    def test_timestamp_boundaries_and_empty_lines(self):
        timestamps = [0, 1, 999999999, 1000000000, 1700000000123456789, 2**64 - 1]
        stream = decode(new_message([(n, "") for n in timestamps]).encode()).streams[0]
        self.assertEqual(values(stream), [(n, "") for n in timestamps])
        for entry in stream.entries:
            self.assertTrue(entry.HasField("timestamp"))
            self.assertLess(entry.timestamp.nanos, 1_000_000_000)

    def test_unicode_nulls_and_escaping(self):
        text = 'quote"\\\n\0café 🐍'
        message = new_message([(1, text)], {"clé\0": text})
        stream = decode(message.encode()).streams[0]
        self.assertEqual(stream.labels, '{"clé\\u0000"="quote\\"\\\\\\u000a\\u0000café 🐍"}')
        self.assertEqual(values(stream), [(1, text)])

    def test_labels_stable_sort_and_duplicates(self):
        stream = Message().stream().label("b", "1").label("a", "2").label("a", "1").entry(1, "x")
        stream.commit()
        message = new_message(labels=[("b", "1"), ("a", "2"), ("a", "1")])
        self.assertEqual(decode(message.encode()).streams[0].labels, '{a="2",a="1",b="1"}')
        self.assertIs(lokiverse.Labels, list)
        self.assertEqual(decode(new_message(labels=[]).encode()).streams[0].labels, "{}")

    def test_order_and_separate_equal_label_sets(self):
        message = new_message([(2, "second"), (1, "first"), (1, "again")])
        message.stream().label("job", "python").entry(0, "separate").commit()
        first = message.encode()
        self.assertEqual(first, message.encode())
        streams = decode(first).streams
        self.assertEqual(len(streams), 2)
        self.assertEqual(values(streams[0]), [(2, "second"), (1, "first"), (1, "again")])
        self.assertEqual(values(streams[1]), [(0, "separate")])

    def test_empty_and_uncommitted_message(self):
        message = Message()
        self.assertIsNone(message.encode())
        message.stream().label("job", "drop").entry(1, "drop")
        gc.collect()
        self.assertIsNone(message.encode())
        with self.assertRaises(AttributeError):
            message.add_stream

    def test_commit_failure_and_builder_reuse(self):
        message = Message()
        stream = message.stream().label("job", "kept")
        with self.assertRaises(ValueError):
            stream.commit()
        self.assertIsNone(message.encode())
        self.assertIsNone(stream.entry(1, "kept").commit())
        with self.assertRaises(ValueError):
            stream.commit()
        stream.reserve(1, 1).entry(2, "second").commit()
        streams = decode(message.encode()).streams
        self.assertEqual(streams[0].labels, '{job="kept"}')
        self.assertEqual(streams[1].labels, "{}")

    def test_reserve_after_entries_and_beyond_hint(self):
        message = Message()
        stream = message.stream()
        self.assertIs(stream.reserve(0, 0), stream)
        stream.reserve(1, 1).label("job", "kept").entry(1, "first")
        stream.reserve(label_count=8, value_count=256).reserve(0, 0)
        for i in range(1, 300):
            stream.entry(i + 1, str(i))
        stream.commit()
        entries = values(decode(message.encode()).streams[0])
        self.assertEqual(len(entries), 300)
        self.assertEqual(entries[0], (1, "first"))
        self.assertEqual(entries[-1], (300, "299"))

    def test_reserve_validation_preserves_builder(self):
        message = Message()
        stream = message.stream().label("job", "kept").entry(1, "kept")
        for bad, kind in [(True, TypeError), (1.5, TypeError), ("1", TypeError), (-1, ValueError), (2**100, OverflowError), (2**64 - 1, OverflowError)]:
            for position in (0, 1):
                with self.subTest(bad=bad, position=position), self.assertRaises(kind):
                    stream.reserve(bad if position == 0 else 0, bad if position == 1 else 0)
        stream.commit()
        self.assertEqual(values(decode(message.encode()).streams[0]), [(1, "kept")])

    def test_entry_argument_validation_preserves_builder(self):
        message = Message()
        stream = message.stream().entry(1, "kept")
        for bad, kind in [(True, TypeError), (1.5, TypeError), ("1", TypeError), (-1, OverflowError), (2**64, OverflowError)]:
            with self.subTest(bad=bad), self.assertRaises(kind):
                stream.entry(bad, "bad")
        for args in [(), (1,), (1, None), (1, b"bytes"), (None, "bad")]:
            with self.subTest(args=args), self.assertRaises(TypeError):
                stream.entry(*args)
        for method, args in [(stream.entry, (1, "\ud800")), (stream.label, ("\ud800", "a")), (stream.label, ("a", "\ud800"))]:
            with self.assertRaises(UnicodeEncodeError):
                method(*args)
        with self.assertRaises(TypeError):
            stream.label("a", 1)
        stream.commit()
        self.assertEqual(values(decode(message.encode()).streams[0]), [(1, "kept")])

    def test_entry_now_and_keywords_capture_call_time(self):
        message = Message()
        stream = message.stream()
        before = time.time_ns()
        stream.entry("a\0b").entry(line="keyword")
        after = time.time_ns()
        time.sleep(0.001)
        stream.entry(timestamp_ns=0, line="epoch").commit()
        entries = values(decode(message.encode()).streams[0])
        for timestamp, _ in entries[:2]:
            self.assertLessEqual(before, timestamp)
            self.assertLessEqual(timestamp, after)
        self.assertEqual([line for _, line in entries], ["a\0b", "keyword", "epoch"])
        self.assertEqual(entries[2][0], 0)

    def test_builder_retains_message(self):
        message = Message()
        count = sys.getrefcount(message)
        stream = message.stream()
        self.assertEqual(sys.getrefcount(message), count + 1)
        del message
        gc.collect()
        stream.entry(1, "alive").commit()
        del stream
        gc.collect()
        with self.assertRaises(TypeError):
            StreamBuilder()

    def test_value_record_and_now(self):
        before = time.time_ns()
        value = Value.now(line="a\0café")
        after = time.time_ns()
        self.assertLessEqual(before, value.timestamp_ns)
        self.assertLessEqual(value.timestamp_ns, after)
        self.assertEqual(value.line, "a\0café")
        value.timestamp_ns = 2**64 - 1
        value.line = "changed"
        self.assertEqual((value.timestamp_ns, value.line), (2**64 - 1, "changed"))
        self.assertEqual(Value(0, "").timestamp_ns, 0)
        for bad, kind in [(True, TypeError), (-1, OverflowError), (2**64, OverflowError)]:
            with self.assertRaises(kind):
                value.timestamp_ns = bad
        with self.assertRaises(TypeError):
            value.line = 1
        with self.assertRaises(UnicodeEncodeError):
            Value.now("\ud800")

    def test_large_payload(self):
        big = "é" * (512 * 1024)
        message = new_message([(1, big)])
        stream = message.stream().reserve(1, 10000).label("job", "many")
        for index in range(10000):
            stream.entry(index, "line")
        stream.commit()
        streams = decode(message.encode()).streams
        self.assertEqual(values(streams[0]), [(1, big)])
        self.assertEqual(len(streams[1].entries), 10000)


class LoggerTest(unittest.TestCase):
    def test_http_statuses_both_factories(self):
        message = new_message()
        for safe in (False, True):
            for status in (200, 204, 400, 500):
                with self.subTest(safe=safe, status=status), receiver(status) as (server, requests):
                    logger = http_logger("127.0.0.1", server.server_port, safe)
                    self.assertIs(logger.push(message=message), status == 204)
                    self.assertEqual(requests.get(timeout=3)[2], message.encode())

    def test_custom_transport_and_empty_message(self):
        for factory in (Logger.make_safe, Logger.make_unsafe):
            sink = Sink()
            logger = factory(transport=sink)
            self.assertFalse(logger.push(Message()))
            self.assertEqual(sink.requests, [])
            message = new_message()
            self.assertTrue(logger.push(message))
            self.assertEqual(sink.requests, [("/loki/api/v1/push", message.encode(), "application/x-protobuf")])
            sink.status = -1
            self.assertFalse(logger.push(message))

    def test_custom_logger(self):
        class Custom(Logger):
            def push(self, message):
                return message.encode() is not None
        self.assertFalse(Custom().push(Message()))
        self.assertTrue(Custom().push(new_message()))

    def test_transport_post(self):
        with receiver(202) as (server, requests):
            transport = Transport.make_http(host="127.0.0.1", port=server.server_port)
            self.assertEqual(transport.post("/custom", b"a\0b", "custom/type"), 202)
            self.assertEqual(requests.get(timeout=3), ("/custom", "custom/type", b"a\0b"))
            with self.assertRaises(TypeError):
                transport.post("/custom", "text", "custom/type")

    def test_callback_exceptions_and_return_validation(self):
        class Failure(Transport):
            def post(self, *args):
                raise LookupError("callback error")
        for factory in (Logger.make_safe, Logger.make_unsafe):
            logger = factory(Failure())
            for _ in range(2):
                with self.assertRaisesRegex(LookupError, "callback error"):
                    logger.push(new_message())
        for status, kind in [(None, TypeError), (True, TypeError), ("204", TypeError), (2**100, OverflowError)]:
            with self.subTest(status=status), self.assertRaises(kind):
                Logger.make_safe(Sink(status)).push(new_message())

    def test_transport_and_logger_validation(self):
        for kind in (Transport, Logger):
            with self.assertRaises(TypeError):
                kind()
        with self.assertRaises(TypeError):
            Logger("localhost", 3100)
        for host in ("", "local\0host"):
            with self.assertRaises(ValueError):
                Transport.make_http(host, 3100)
        for port in (-1, 0, 65536, 2**100):
            with self.assertRaises(ValueError):
                Transport.make_http("localhost", port)
        for port in (True, 1.5, "3100"):
            with self.assertRaises(TypeError):
                Transport.make_http("localhost", port)
        for port in (1, 65535):
            self.assertIsInstance(Transport.make_http("localhost", port), Transport)
        for factory in (Logger.make_safe, Logger.make_unsafe):
            with self.assertRaises(TypeError):
                factory(None)
        with self.assertRaises(TypeError):
            Logger.make_safe(Sink()).push({})
        class MissingTransport(Transport):
            pass
        with self.assertRaises(NotImplementedError):
            Logger.make_safe(MissingTransport()).push(new_message())
        class MissingLogger(Logger):
            pass
        with self.assertRaises(NotImplementedError):
            MissingLogger().push(new_message())

    def test_safe_logger_serializes_custom_transport(self):
        sink = Sink(delay=0.005)
        logger = Logger.make_safe(sink)
        message = new_message()
        with ThreadPoolExecutor(max_workers=8) as workers:
            self.assertEqual(list(workers.map(lambda _: logger.push(message), range(24))), [True] * 24)
        self.assertEqual(sink.maximum, 1)
        self.assertEqual(len(sink.requests), 24)

    def test_snapshot_while_committing_during_io(self):
        release = threading.Event()
        message = new_message()
        with receiver(release=release) as (server, requests):
            logger = http_logger("127.0.0.1", server.server_port)
            results = []
            thread = threading.Thread(target=lambda: results.append(logger.push(message)))
            thread.start()
            first = decode(requests.get(timeout=3)[2])
            message.stream().label("job", "later").entry(2, "later").commit()
            release.set()
            thread.join(timeout=3)
            self.assertFalse(thread.is_alive())
            self.assertEqual(results, [True])
            self.assertTrue(logger.push(message))
            second = decode(requests.get(timeout=3)[2])
        self.assertEqual(len(first.streams), 1)
        self.assertEqual(len(second.streams), 2)

    def test_transport_and_logger_cycles_are_collected(self):
        sink = Sink()
        logger = Logger.make_safe(sink)
        sink.logger = logger
        reference = weakref.ref(sink)
        del sink, logger
        gc.collect()
        self.assertIsNone(reference())

    def test_native_logger_keeps_transport_alive(self):
        sink = Sink()
        reference = weakref.ref(sink)
        logger = Logger.make_safe(sink)
        del sink
        gc.collect()
        self.assertIsNotNone(reference())
        self.assertTrue(logger.push(new_message()))
        del logger
        gc.collect()
        self.assertIsNone(reference())

    def test_connection_failure(self):
        with receiver() as (server, _):
            port = server.server_port
        self.assertFalse(http_logger("127.0.0.1", port).push(new_message()))


class LifecycleTest(unittest.TestCase):
    def test_final_value_message_and_builder(self):
        for kind in (Message, Value, StreamBuilder):
            with self.assertRaises(TypeError):
                type("Derived", (kind,), {})
        for instance in (Message(), Value(0, ""), Message().stream()):
            with self.assertRaises(AttributeError):
                instance.extra = 1
            for method in (pickle.dumps, copy.copy, copy.deepcopy):
                with self.assertRaises(TypeError):
                    method(instance)

    def test_uninitialized_objects_and_reinitialization(self):
        message = Message.__new__(Message)
        for method in (message.stream, message.encode):
            with self.assertRaises(RuntimeError):
                method()
        with self.assertRaises(RuntimeError):
            Logger.make_safe(Sink()).push(message)
        message.__init__()
        with self.assertRaises(RuntimeError):
            message.__init__()
        value = Value.__new__(Value)
        with self.assertRaises(RuntimeError):
            _ = value.line
        value.__init__(0, "")
        with self.assertRaises(RuntimeError):
            value.__init__(1, "again")

    def test_signatures_and_docs(self):
        self.assertEqual(str(inspect.signature(Message)), "()")
        self.assertEqual(str(inspect.signature(Message.stream)), "(self, /)")
        self.assertEqual(str(inspect.signature(StreamBuilder.reserve)), "(self, /, label_count, value_count)")
        self.assertEqual(str(inspect.signature(Transport.make_http)), "(host, port)")
        self.assertEqual(str(inspect.signature(Logger.make_safe)), "(transport)")
        for kind in (Message, StreamBuilder, Transport, Logger, Value):
            self.assertTrue(kind.__doc__)
            self.assertEqual(kind.__module__, "lokiverse")
        for method in (Message.stream, Message.encode, StreamBuilder.reserve, StreamBuilder.label, StreamBuilder.entry, StreamBuilder.commit, Transport.make_http, Transport.post, Logger.make_safe, Logger.make_unsafe, Logger.push, Value.now):
            self.assertTrue(method.__doc__)

    def test_reference_counts_are_stable(self):
        key, text, line = "key-" * 8, "value-" * 8, "line-" * 8
        message = Message()
        sink = Sink()
        logger = Logger.make_safe(sink)
        tracked = [key, text, line, message, sink]
        def exercise():
            stream = message.stream().reserve(1, 1).label(key, text).entry(1, line)
            with self.assertRaises(TypeError):
                stream.entry(1, None)
            stream.commit()
        exercise()
        before = [sys.getrefcount(item) for item in tracked]
        for _ in range(50):
            exercise()
        gc.collect()
        self.assertEqual([sys.getrefcount(item) for item in tracked], before)
        self.assertTrue(logger.push(message))

    def test_reimported_module_has_distinct_types(self):
        result = run_child('''
            import gc, sys
            import lokiverse as first
            del sys.modules["lokiverse"]
            import lokiverse as second
            assert first.Message is not second.Message
            message = first.Message()
            message.stream().entry(1, "hello").commit()
            logger = second.Logger.make_safe(second.Transport.make_http("localhost", 1))
            try:
                logger.push(message)
            except TypeError:
                pass
            else:
                raise AssertionError("foreign message accepted")
            del first, message
            gc.collect()
            assert logger.push(second.Message()) is False
        ''')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_subinterpreters_are_rejected(self):
        try:
            import _interpreters
        except ImportError:
            self.skipTest("_interpreters is unavailable")
        result = run_child('''
            import _interpreters
            interpreter = _interpreters.create()
            try:
                failure = _interpreters.exec(interpreter, "import lokiverse")
            finally:
                _interpreters.destroy(interpreter)
            assert failure.type.__name__ == "ImportError"
            import lokiverse
            message = lokiverse.Message()
            message.stream().entry(1, "main interpreter").commit()
            assert message.encode()
        ''')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_blocking_callback_releases_gil(self):
        result = run_child('''
            from concurrent.futures import ThreadPoolExecutor
            import lokiverse
            import time
            class Transport(lokiverse.Transport):
                def post(self, path, body, content_type):
                    time.sleep(0.01)
                    return 204
            logger = lokiverse.Logger.make_safe(Transport())
            message = lokiverse.Message()
            message.stream().entry(1, "hello").commit()
            with ThreadPoolExecutor(max_workers=4) as workers:
                assert all(workers.map(lambda _: logger.push(message), range(8)))
        ''')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
