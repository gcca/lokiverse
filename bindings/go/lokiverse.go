package lokiverse

/*
#include <stdlib.h>
#include "lokiverse.h"
*/
import "C"

import (
	"errors"
	"runtime"
	"slices"
	"sync"
	"time"
	"unicode/utf8"
	"unsafe"
)

var (
	ErrClosed          = errors.New("lokiverse: use of closed handle")
	ErrNoEntries       = errors.New("lokiverse: stream has no entries")
	ErrInvalidUTF8     = errors.New("lokiverse: text is not valid UTF-8")
	ErrInvalidArgument = errors.New("lokiverse: invalid argument")
	ErrNoMemory        = errors.New("lokiverse: out of memory")
	ErrInternal        = errors.New("lokiverse: internal error")
)

func statusError(status C.int) error {
	switch status {
	case C.LOKIVERSE_OK:
		return nil
	case C.LOKIVERSE_INVALID_ARGUMENT:
		return ErrInvalidArgument
	case C.LOKIVERSE_NO_MEMORY:
		return ErrNoMemory
	default:
		return ErrInternal
	}
}

func chars(text string) (*C.char, C.size_t) {
	return (*C.char)(unsafe.Pointer(unsafe.StringData(text))), C.size_t(len(text))
}

type label struct {
	name  string
	value string
}

type entry struct {
	timestamp uint64
	line      string
}

type Value struct {
	TimestampNS uint64
	Line        string
}

func Now(line string) Value {
	return Value{TimestampNS: uint64(time.Now().UnixNano()), Line: line}
}

type Label struct {
	Name  string
	Value string
}

type Labels []Label

type Message struct {
	mutex  sync.RWMutex
	handle *C.lokiverse_message
}

func NewMessage() *Message {
	handle := C.lokiverse_message_new()
	if handle == nil {
		panic(ErrNoMemory)
	}
	message := &Message{handle: handle}
	runtime.SetFinalizer(message, (*Message).Close)
	return message
}

func (m *Message) Close() {
	m.mutex.Lock()
	defer m.mutex.Unlock()
	if m.handle != nil {
		C.lokiverse_message_free(m.handle)
		m.handle = nil
	}
	runtime.SetFinalizer(m, nil)
}

func (m *Message) Stream() *StreamBuilder {
	return &StreamBuilder{message: m}
}

func (m *Message) Encode() ([]byte, bool) {
	m.mutex.RLock()
	defer m.mutex.RUnlock()
	if m.handle == nil {
		return nil, false
	}
	var data *C.char
	var size C.size_t
	if C.lokiverse_message_encode(m.handle, &data, &size) != C.LOKIVERSE_OK {
		return nil, false
	}
	defer C.lokiverse_buffer_free(data)
	return C.GoBytes(unsafe.Pointer(data), C.int(size)), true
}

type StreamBuilder struct {
	message *Message
	labels  []label
	entries []entry
}

func (s *StreamBuilder) Reserve(labelCount, valueCount int) *StreamBuilder {
	if labelCount < 0 || valueCount < 0 {
		panic(ErrInvalidArgument)
	}
	if labelCount > cap(s.labels) {
		s.labels = slices.Grow(s.labels, labelCount-len(s.labels))
	}
	if valueCount > cap(s.entries) {
		s.entries = slices.Grow(s.entries, valueCount-len(s.entries))
	}
	return s
}

func (s *StreamBuilder) Label(name, value string) *StreamBuilder {
	s.labels = append(s.labels, label{name, value})
	return s
}

func (s *StreamBuilder) Entry(timestampNs uint64, line string) *StreamBuilder {
	s.entries = append(s.entries, entry{timestampNs, line})
	return s
}

func (s *StreamBuilder) EntryNow(line string) *StreamBuilder {
	value := Now(line)
	return s.Entry(value.TimestampNS, value.Line)
}

func (s *StreamBuilder) Commit() error {
	if len(s.entries) == 0 {
		return ErrNoEntries
	}
	for _, item := range s.labels {
		if !utf8.ValidString(item.name) || !utf8.ValidString(item.value) {
			return ErrInvalidUTF8
		}
	}
	for _, item := range s.entries {
		if !utf8.ValidString(item.line) {
			return ErrInvalidUTF8
		}
	}
	m := s.message
	m.mutex.Lock()
	defer m.mutex.Unlock()
	if m.handle == nil {
		return ErrClosed
	}
	stream := C.lokiverse_message_stream(m.handle)
	if stream == nil {
		return ErrNoMemory
	}
	defer C.lokiverse_stream_free(stream)
	if err := statusError(C.lokiverse_stream_reserve(stream, C.size_t(len(s.labels)), C.size_t(len(s.entries)))); err != nil {
		return err
	}
	for _, item := range s.labels {
		name, nameSize := chars(item.name)
		value, valueSize := chars(item.value)
		if err := statusError(C.lokiverse_stream_label(stream, name, nameSize, value, valueSize)); err != nil {
			return err
		}
	}
	for _, item := range s.entries {
		line, lineSize := chars(item.line)
		if err := statusError(C.lokiverse_stream_entry(stream, C.uint64_t(item.timestamp), line, lineSize)); err != nil {
			return err
		}
	}
	if err := statusError(C.lokiverse_stream_commit(stream)); err != nil {
		return err
	}
	runtime.KeepAlive(s)
	s.labels = nil
	s.entries = nil
	return nil
}

type Transport interface {
	Post(path string, body []byte, contentType string) int
}

type Logger interface {
	Push(message *Message) bool
}

type HTTPTransport struct {
	mutex  sync.RWMutex
	handle *C.lokiverse_transport
}

func MakeHTTP(host string, port int) (*HTTPTransport, error) {
	if port < 1 || port > 65535 {
		return nil, ErrInvalidArgument
	}
	data, size := chars(host)
	var handle *C.lokiverse_transport
	if err := statusError(C.lokiverse_transport_make_http(data, size, C.int(port), &handle)); err != nil {
		return nil, err
	}
	runtime.KeepAlive(host)
	transport := &HTTPTransport{handle: handle}
	runtime.SetFinalizer(transport, (*HTTPTransport).Close)
	return transport, nil
}

func (t *HTTPTransport) Close() {
	t.mutex.Lock()
	defer t.mutex.Unlock()
	if t.handle != nil {
		C.lokiverse_transport_free(t.handle)
		t.handle = nil
	}
	runtime.SetFinalizer(t, nil)
}

func (t *HTTPTransport) Post(path string, body []byte, contentType string) int {
	t.mutex.RLock()
	defer t.mutex.RUnlock()
	if t.handle == nil {
		return -1
	}
	p, pn := chars(path)
	ct, cn := chars(contentType)
	var status C.int
	result := C.lokiverse_transport_post(t.handle, p, pn, (*C.char)(unsafe.Pointer(unsafe.SliceData(body))), C.size_t(len(body)), ct, cn, &status)
	runtime.KeepAlive(path)
	runtime.KeepAlive(body)
	runtime.KeepAlive(contentType)
	if result != C.LOKIVERSE_OK {
		return -1
	}
	return int(status)
}

type NativeLogger struct {
	mutex  sync.RWMutex
	handle *C.lokiverse_logger
}

func MakeSafe(transport Transport) (*NativeLogger, error)   { return makeLogger(transport, true) }
func MakeUnsafe(transport Transport) (*NativeLogger, error) { return makeLogger(transport, false) }

func makeLogger(transport Transport, safe bool) (*NativeLogger, error) {
	if transport == nil {
		return nil, ErrInvalidArgument
	}
	var handle *C.lokiverse_transport
	if http, ok := transport.(*HTTPTransport); ok {
		if http == nil {
			return nil, ErrInvalidArgument
		}
		http.mutex.RLock()
		defer http.mutex.RUnlock()
		if http.handle == nil {
			return nil, ErrClosed
		}
		handle = http.handle
	} else {
		var err error
		handle, err = customTransport(transport)
		if err != nil {
			return nil, err
		}
		defer C.lokiverse_transport_free(handle)
	}
	var loggerHandle *C.lokiverse_logger
	var status C.int
	if safe {
		status = C.lokiverse_logger_make_safe(handle, &loggerHandle)
	} else {
		status = C.lokiverse_logger_make_unsafe(handle, &loggerHandle)
	}
	if err := statusError(status); err != nil {
		return nil, err
	}
	logger := &NativeLogger{handle: loggerHandle}
	runtime.SetFinalizer(logger, (*NativeLogger).Close)
	return logger, nil
}

func (l *NativeLogger) Close() {
	l.mutex.Lock()
	defer l.mutex.Unlock()
	if l.handle != nil {
		C.lokiverse_logger_free(l.handle)
		l.handle = nil
	}
	runtime.SetFinalizer(l, nil)
}

func (l *NativeLogger) Push(message *Message) bool {
	if message == nil {
		return false
	}
	l.mutex.RLock()
	defer l.mutex.RUnlock()
	message.mutex.RLock()
	defer message.mutex.RUnlock()
	if l.handle == nil || message.handle == nil {
		return false
	}
	var success C.int
	status := C.lokiverse_logger_push(l.handle, message.handle, &success)
	runtime.KeepAlive(l)
	runtime.KeepAlive(message)
	return status == C.LOKIVERSE_OK && success != 0
}
