package lokiverse

import (
	"bytes"
	"errors"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

type request struct {
	path        string
	contentType string
	body        []byte
}

func receiver(t *testing.T, status int) (int, chan request) {
	t.Helper()
	requests := make(chan request, 64)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		requests <- request{r.URL.Path, r.Header.Get("Content-Type"), body}
		w.WriteHeader(status)
	}))
	t.Cleanup(server.Close)
	return server.Listener.Addr().(*net.TCPAddr).Port, requests
}

func newMessage(t *testing.T) *Message {
	t.Helper()
	message := NewMessage()
	t.Cleanup(message.Close)
	return message
}

func httpLogger(host string, port int) (*NativeLogger, error) {
	transport, err := MakeHTTP(host, port)
	if err != nil {
		return nil, err
	}
	defer transport.Close()
	return MakeSafe(transport)
}

func newLogger(t *testing.T, port int) *NativeLogger {
	t.Helper()
	logger, err := httpLogger("127.0.0.1", port)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(logger.Close)
	return logger
}

func decoded(t *testing.T, message *Message) []wireStream {
	t.Helper()
	body, ok := message.Encode()
	if !ok {
		t.Fatal("Encode returned no body")
	}
	streams, err := decodeWire(body)
	if err != nil {
		t.Fatal(err)
	}
	return streams
}

func pairs(stream wireStream) []entry {
	var result []entry
	for _, item := range stream.entries {
		result = append(result, entry{item.timestamp, item.line})
	}
	return result
}

func expectEntries(t *testing.T, stream wireStream, expected ...entry) {
	t.Helper()
	actual := pairs(stream)
	if len(actual) != len(expected) {
		t.Fatalf("entries = %v, expected %v", actual, expected)
	}
	for i := range expected {
		if actual[i] != expected[i] {
			t.Fatalf("entry %d = %v, expected %v", i, actual[i], expected[i])
		}
	}
}

func TestEncodesMultipleStreams(t *testing.T) {
	message := newMessage(t)
	err := message.Stream().
		Label("job", "api").
		Label("env", "test").
		Entry(1700000000123456789, "first").
		Entry(1700000000123456790, "second").
		Commit()
	if err != nil {
		t.Fatal(err)
	}
	if err := message.Stream().Label("job", "worker").Entry(1700000001123456789, "third").Commit(); err != nil {
		t.Fatal(err)
	}
	streams := decoded(t, message)
	if len(streams) != 2 {
		t.Fatalf("streams = %d", len(streams))
	}
	if streams[0].labels != `{env="test",job="api"}` || streams[1].labels != `{job="worker"}` {
		t.Fatalf("labels = %q, %q", streams[0].labels, streams[1].labels)
	}
	expectEntries(t, streams[0], entry{1700000000123456789, "first"}, entry{1700000000123456790, "second"})
	expectEntries(t, streams[1], entry{1700000001123456789, "third"})
}

func TestTimestampBoundariesAndEmptyLines(t *testing.T) {
	timestamps := []uint64{0, 1, 999999999, 1000000000, 1700000000123456789, 1<<64 - 1}
	message := newMessage(t)
	stream := message.Stream()
	for _, timestamp := range timestamps {
		stream.Entry(timestamp, "")
	}
	if err := stream.Commit(); err != nil {
		t.Fatal(err)
	}
	streams := decoded(t, message)
	if streams[0].labels != "{}" {
		t.Fatalf("labels = %q", streams[0].labels)
	}
	if len(streams[0].entries) != len(timestamps) {
		t.Fatalf("entries = %d", len(streams[0].entries))
	}
	for i, item := range streams[0].entries {
		if !item.hasTimestamp || item.nanos >= 1000000000 || item.timestamp != timestamps[i] || item.line != "" {
			t.Fatalf("entry %d = %+v", i, item)
		}
	}
}

func TestUnicodeNullsAndEscapedLabels(t *testing.T) {
	line := "say \"hi\"\\\nnext\ttab\x00fin: café 🐍"
	message := newMessage(t)
	if err := message.Stream().Label("clé\x00", "quote\"\\\n\x00café 🐍").Entry(1, line).Commit(); err != nil {
		t.Fatal(err)
	}
	streams := decoded(t, message)
	if streams[0].labels != `{"clé\u0000"="quote\"\\\u000a\u0000café 🐍"}` {
		t.Fatalf("labels = %q", streams[0].labels)
	}
	expectEntries(t, streams[0], entry{1, line})
}

func TestEntryNowUsesWallClock(t *testing.T) {
	message := newMessage(t)
	before := uint64(time.Now().UnixNano())
	stream := message.Stream().EntryNow("now")
	after := uint64(time.Now().UnixNano())
	time.Sleep(2 * time.Millisecond)
	if err := stream.Commit(); err != nil {
		t.Fatal(err)
	}
	item := decoded(t, message)[0].entries[0]
	if item.timestamp < before || item.timestamp > after || item.line != "now" {
		t.Fatalf("entry = %+v outside [%d, %d]", item, before, after)
	}
}

func TestCommitRequiresEntriesAndBuilderIsReusable(t *testing.T) {
	message := newMessage(t)
	stream := message.Stream().Label("job", "go")
	if err := stream.Commit(); !errors.Is(err, ErrNoEntries) {
		t.Fatalf("Commit = %v", err)
	}
	if _, ok := message.Encode(); ok {
		t.Fatal("failed Commit changed the message")
	}
	message.Stream().Label("job", "discarded").Entry(1, "discarded")
	if _, ok := message.Encode(); ok {
		t.Fatal("uncommitted builder changed the message")
	}
	if err := stream.Entry(2, "second").Entry(1, "first").Commit(); err != nil {
		t.Fatal(err)
	}
	if err := stream.Commit(); !errors.Is(err, ErrNoEntries) {
		t.Fatalf("Commit after reset = %v", err)
	}
	if err := stream.Label("job", "go").Entry(1, "separate").Commit(); err != nil {
		t.Fatal(err)
	}
	streams := decoded(t, message)
	if len(streams) != 2 || streams[0].labels != `{job="go"}` || streams[1].labels != `{job="go"}` {
		t.Fatalf("streams = %+v", streams)
	}
	expectEntries(t, streams[0], entry{2, "second"}, entry{1, "first"})
	expectEntries(t, streams[1], entry{1, "separate"})
}

func TestInvalidUTF8IsRejected(t *testing.T) {
	message := newMessage(t)
	builders := []*StreamBuilder{
		message.Stream().Label("\xff", "value").Entry(1, "line"),
		message.Stream().Label("name", "\xff").Entry(1, "line"),
		message.Stream().Entry(1, "\xff"),
	}
	for i, builder := range builders {
		if err := builder.Commit(); !errors.Is(err, ErrInvalidUTF8) {
			t.Fatalf("builder %d gave %v", i, err)
		}
	}
	if _, ok := message.Encode(); ok {
		t.Fatal("invalid text changed the message")
	}
}

func TestPushPostsEncodedBody(t *testing.T) {
	port, requests := receiver(t, http.StatusNoContent)
	logger := newLogger(t, port)
	message := newMessage(t)
	if err := message.Stream().Label("job", "go").Entry(1, "hello").Commit(); err != nil {
		t.Fatal(err)
	}
	body, _ := message.Encode()
	for i := 0; i < 2; i++ {
		if !logger.Push(message) {
			t.Fatal("Push failed")
		}
		received := <-requests
		if received.path != "/loki/api/v1/push" || received.contentType != "application/x-protobuf" {
			t.Fatalf("request = %q %q", received.path, received.contentType)
		}
		if !bytes.Equal(received.body, body) {
			t.Fatal("posted body differs from Encode")
		}
	}
}

func TestResultFollowsStatus(t *testing.T) {
	for _, status := range []int{200, 204, 400, 500} {
		port, _ := receiver(t, status)
		message := newMessage(t)
		if err := message.Stream().Entry(1, "hello").Commit(); err != nil {
			t.Fatal(err)
		}
		if result := newLogger(t, port).Push(message); result != (status == 204) {
			t.Fatalf("status %d gave %v", status, result)
		}
	}
}

func TestEmptyMessageDoesNotPost(t *testing.T) {
	port, requests := receiver(t, http.StatusNoContent)
	message := newMessage(t)
	if _, ok := message.Encode(); ok {
		t.Fatal("empty message encoded")
	}
	if newLogger(t, port).Push(message) {
		t.Fatal("empty message pushed")
	}
	if len(requests) != 0 {
		t.Fatal("empty message was posted")
	}
}

func TestConnectionFailureReturnsFalse(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := listener.Addr().(*net.TCPAddr).Port
	listener.Close()
	message := newMessage(t)
	if err := message.Stream().Entry(1, "hello").Commit(); err != nil {
		t.Fatal(err)
	}
	if newLogger(t, port).Push(message) {
		t.Fatal("Push succeeded without a server")
	}
}

func TestLoggerArguments(t *testing.T) {
	for _, host := range []string{"", "local\x00host"} {
		if _, err := httpLogger(host, 3100); !errors.Is(err, ErrInvalidArgument) {
			t.Fatalf("host %q gave %v", host, err)
		}
	}
	for _, port := range []int{-1, 0, 65536, 1 << 40} {
		if _, err := httpLogger("localhost", port); !errors.Is(err, ErrInvalidArgument) {
			t.Fatalf("port %d gave %v", port, err)
		}
	}
	for _, port := range []int{1, 65535} {
		logger, err := httpLogger("localhost", port)
		if err != nil {
			t.Fatalf("port %d gave %v", port, err)
		}
		logger.Close()
	}
}

func TestClosedHandles(t *testing.T) {
	message := NewMessage()
	stream := message.Stream().Entry(1, "hello")
	if err := stream.Commit(); err != nil {
		t.Fatal(err)
	}
	logger, err := httpLogger("localhost", 3100)
	if err != nil {
		t.Fatal(err)
	}
	logger.Close()
	logger.Close()
	if logger.Push(message) {
		t.Fatal("closed logger pushed")
	}
	message.Close()
	message.Close()
	if _, ok := message.Encode(); ok {
		t.Fatal("closed message encoded")
	}
	if err := stream.Entry(2, "late").Commit(); !errors.Is(err, ErrClosed) {
		t.Fatalf("Commit on closed message = %v", err)
	}
	port, _ := receiver(t, http.StatusNoContent)
	if newLogger(t, port).Push(message) || newLogger(t, port).Push(nil) {
		t.Fatal("closed or nil message pushed")
	}
}

func TestConcurrentPushes(t *testing.T) {
	port, requests := receiver(t, http.StatusNoContent)
	logger := newLogger(t, port)
	message := newMessage(t)
	if err := message.Stream().Label("job", "go").Entry(1, "hello").Commit(); err != nil {
		t.Fatal(err)
	}
	body, _ := message.Encode()
	var group sync.WaitGroup
	results := make([]bool, 16)
	for i := range results {
		group.Add(1)
		go func() {
			defer group.Done()
			results[i] = logger.Push(message)
		}()
	}
	group.Wait()
	for i, result := range results {
		if !result {
			t.Fatalf("push %d failed", i)
		}
		if received := <-requests; !bytes.Equal(received.body, body) {
			t.Fatal("posted body differs from Encode")
		}
	}
}

func TestLargePayload(t *testing.T) {
	big := strings.Repeat("é", 512*1024)
	message := newMessage(t)
	if err := message.Stream().Entry(1, big).Commit(); err != nil {
		t.Fatal(err)
	}
	stream := message.Stream().Label("job", "many")
	for i := 0; i < 10000; i++ {
		stream.Entry(uint64(i), "line")
	}
	if err := stream.Commit(); err != nil {
		t.Fatal(err)
	}
	streams := decoded(t, message)
	expectEntries(t, streams[0], entry{1, big})
	if len(streams[1].entries) != 10000 || streams[1].entries[9999].timestamp != 9999 {
		t.Fatalf("entries = %d", len(streams[1].entries))
	}
}

type recordingTransport struct {
	mutex   sync.Mutex
	calls   int
	active  int
	maximum int
	body    []byte
	fail    bool
}

func (r *recordingTransport) Post(path string, body []byte, contentType string) int {
	if r.fail {
		panic("transport panic")
	}
	r.mutex.Lock()
	r.active++
	if r.active > r.maximum {
		r.maximum = r.active
	}
	r.mutex.Unlock()
	time.Sleep(time.Millisecond)
	r.mutex.Lock()
	defer r.mutex.Unlock()
	r.active--
	r.calls++
	r.body = body
	if path != "/loki/api/v1/push" || contentType != "application/x-protobuf" {
		return -1
	}
	return 204
}

type customLogger struct{}

func (customLogger) Push(message *Message) bool { _, ok := message.Encode(); return ok }

func TestReservationAndValue(t *testing.T) {
	m := newMessage(t)
	s := m.Stream().Reserve(0, 0).Reserve(1, 1).Label("job", "kept").Entry(1, "first")
	s.Reserve(8, 256).Reserve(0, 0)
	for i := 0; i < 300; i++ {
		s.Entry(uint64(i), "later")
	}
	if err := s.Commit(); err != nil {
		t.Fatal(err)
	}
	streams := decoded(t, m)
	if len(streams[0].entries) != 301 || streams[0].entries[0].line != "first" {
		t.Fatal("reservation changed contents")
	}
	if err := s.Reserve(1, 1).Entry(1, "reused").Commit(); err != nil {
		t.Fatal(err)
	}
	before := uint64(time.Now().UnixNano())
	value := Now("a\x00b")
	if value.TimestampNS < before || value.TimestampNS > uint64(time.Now().UnixNano()) || value.Line != "a\x00b" {
		t.Fatalf("value=%+v", value)
	}
	defer func() {
		if !errors.Is(recover().(error), ErrInvalidArgument) {
			t.Fatal("negative reserve did not reject")
		}
	}()
	s.Reserve(-1, 0)
}

func TestCustomTransportFactories(t *testing.T) {
	for _, factory := range []func(Transport) (*NativeLogger, error){MakeSafe, MakeUnsafe} {
		transport := &recordingTransport{}
		logger, err := factory(transport)
		if err != nil {
			t.Fatal(err)
		}
		message := newMessage(t)
		if logger.Push(message) || transport.calls != 0 {
			t.Fatal("empty message posted")
		}
		if err := message.Stream().Entry(1, "custom").Commit(); err != nil {
			t.Fatal(err)
		}
		if !logger.Push(message) {
			t.Fatal("custom transport failed")
		}
		body, _ := message.Encode()
		if !bytes.Equal(body, transport.body) {
			t.Fatal("custom body mismatch")
		}
		transport.fail = true
		if logger.Push(message) {
			t.Fatal("panic was not contained")
		}
		logger.Close()
	}
	var logger Logger = customLogger{}
	if logger.Push(newMessage(t)) {
		t.Fatal("custom logger")
	}
	if _, err := MakeSafe(nil); !errors.Is(err, ErrInvalidArgument) {
		t.Fatal(err)
	}
}

func TestCustomSafeTransportConcurrency(t *testing.T) {
	transport := &recordingTransport{}
	logger, err := MakeSafe(transport)
	if err != nil {
		t.Fatal(err)
	}
	defer logger.Close()
	message := newMessage(t)
	if err := message.Stream().Entry(1, "shared").Commit(); err != nil {
		t.Fatal(err)
	}
	var workers sync.WaitGroup
	for i := 0; i < 16; i++ {
		workers.Add(1)
		go func() {
			defer workers.Done()
			if !logger.Push(message) {
				t.Error("push failed")
			}
		}()
	}
	workers.Wait()
	if transport.maximum != 1 || transport.calls != 16 {
		t.Fatalf("active=%d calls=%d", transport.maximum, transport.calls)
	}
}

func TestDirectHTTPTransport(t *testing.T) {
	port, requests := receiver(t, 202)
	transport, err := MakeHTTP("127.0.0.1", port)
	if err != nil {
		t.Fatal(err)
	}
	if transport.Post("/direct", []byte("a\x00b"), "custom/type") != 202 {
		t.Fatal("post failed")
	}
	request := <-requests
	if request.path != "/direct" || request.contentType != "custom/type" || !bytes.Equal(request.body, []byte("a\x00b")) {
		t.Fatal(request)
	}
	transport.Close()
	if transport.Post("/direct", nil, "custom/type") != -1 {
		t.Fatal("closed post")
	}
	if _, err := MakeSafe(transport); !errors.Is(err, ErrClosed) {
		t.Fatal(err)
	}
}
