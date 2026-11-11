#include <Python.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "lokiverse.hpp"

namespace {

extern PyModuleDef Module;

struct ModuleState {
  PyTypeObject *message_type;
  PyTypeObject *stream_type;
  PyTypeObject *transport_type;
  PyTypeObject *logger_type;
  PyTypeObject *value_type;
};

struct LoggerState {
  std::unique_ptr<lokiverse::Transport> adapter;
  std::unique_ptr<lokiverse::Logger> logger;
};

struct PythonMessage {
  PyObject_HEAD lokiverse::Message *message;
};

struct PythonStream {
  PyObject_HEAD lokiverse::Message::StreamBuilder *builder;
  PyObject *owner;
};

struct PythonTransport {
  PyObject_HEAD lokiverse::Transport *transport;
};

struct PythonLogger {
  PyObject_HEAD LoggerState *state;
  PyObject *owner;
};

struct PythonValue {
  PyObject_HEAD lokiverse::Value *value;
};

class PyRef {
public:
  explicit PyRef(PyObject *object) : object_(object) {}
  ~PyRef() { Py_XDECREF(object_); }
  PyRef(const PyRef &) = delete;
  PyRef &operator=(const PyRef &) = delete;
  PyObject *Get() const { return object_; }

private:
  PyObject *object_;
};

class AllowThreads {
public:
  AllowThreads() : state_(PyEval_SaveThread()) {}
  ~AllowThreads() { PyEval_RestoreThread(state_); }
  AllowThreads(const AllowThreads &) = delete;
  AllowThreads &operator=(const AllowThreads &) = delete;

private:
  PyThreadState *state_;
};

class AcquireGIL {
public:
  AcquireGIL() : state_(PyGILState_Ensure()) {}
  ~AcquireGIL() { PyGILState_Release(state_); }
  AcquireGIL(const AcquireGIL &) = delete;
  AcquireGIL &operator=(const AcquireGIL &) = delete;

private:
  PyGILState_STATE state_;
};

class PythonException {
public:
  PythonException()
      : error_(PyErr_GetRaisedException(), [](PyObject *error) {
          AcquireGIL gil;
          Py_XDECREF(error);
        }) {}

  void Restore() const { PyErr_SetRaisedException(Py_XNewRef(error_.get())); }

private:
  std::shared_ptr<PyObject> error_;
};

void SetCppException() {
  try {
    throw;
  } catch (const PythonException &error) {
    error.Restore();
  } catch (const std::bad_alloc &) {
    PyErr_NoMemory();
  } catch (const std::length_error &error) {
    PyErr_SetString(PyExc_OverflowError, error.what());
  } catch (const std::invalid_argument &error) {
    PyErr_SetString(PyExc_ValueError, error.what());
  } catch (const std::exception &error) {
    PyErr_SetString(PyExc_RuntimeError, error.what());
  } catch (...) {
    PyErr_SetString(PyExc_RuntimeError, "unexpected native exception");
  }
}

ModuleState *State(PyTypeObject *type) {
  PyObject *module = PyType_GetModuleByDef(type, &Module);
  return module ? static_cast<ModuleState *>(PyModule_GetState(module))
                : nullptr;
}

bool CopyString(PyObject *object, std::string &output, const char *error) {
  if (!PyUnicode_Check(object)) {
    PyErr_SetString(PyExc_TypeError, error);
    return false;
  }
  Py_ssize_t size;
  const char *data = PyUnicode_AsUTF8AndSize(object, &size);
  if (!data) {
    return false;
  }
  output.assign(data, size);
  return true;
}

bool Timestamp(PyObject *object, std::uint64_t &value) {
  if (!PyLong_Check(object) || PyBool_Check(object)) {
    PyErr_SetString(PyExc_TypeError, "timestamp_ns must be an int");
    return false;
  }
  value = PyLong_AsUnsignedLongLong(object);
  return !PyErr_Occurred();
}

bool Count(PyObject *object, std::size_t &value) {
  if (!PyLong_Check(object) || PyBool_Check(object)) {
    PyErr_SetString(PyExc_TypeError, "capacity must be an int");
    return false;
  }
  int overflow;
  const auto signed_value = PyLong_AsLongLongAndOverflow(object, &overflow);
  if (PyErr_Occurred()) {
    return false;
  }
  if (overflow < 0 || (overflow == 0 && signed_value < 0)) {
    PyErr_SetString(PyExc_ValueError, "capacity must not be negative");
    return false;
  }
  value = PyLong_AsSize_t(object);
  return !PyErr_Occurred();
}

class CallbackTransport final : public lokiverse::Transport {
public:
  explicit CallbackTransport(PyObject *owner) : owner_(owner) {}

  int Post(std::string_view path, std::string_view body,
           std::string_view content_type) override {
    AcquireGIL gil;
    PyRef method(PyObject_GetAttrString(owner_, "post"));
    PyRef py_path(PyUnicode_DecodeUTF8(path.data(), path.size(), "strict"));
    PyRef py_body(PyBytes_FromStringAndSize(body.data(), body.size()));
    PyRef py_type(PyUnicode_DecodeUTF8(content_type.data(), content_type.size(),
                                       "strict"));
    if (!method.Get() || !py_path.Get() || !py_body.Get() || !py_type.Get()) {
      throw PythonException();
    }
    PyRef result(PyObject_CallFunctionObjArgs(
        method.Get(), py_path.Get(), py_body.Get(), py_type.Get(), nullptr));
    if (!result.Get()) {
      throw PythonException();
    }
    if (!PyLong_Check(result.Get()) || PyBool_Check(result.Get())) {
      PyErr_SetString(PyExc_TypeError,
                      "post must return an int HTTP status or -1");
      throw PythonException();
    }
    const auto status = PyLong_AsLong(result.Get());
    if (PyErr_Occurred()) {
      throw PythonException();
    }
    if (status < -1 || status > 2147483647) {
      PyErr_SetString(PyExc_OverflowError,
                      "post status is outside the int range");
      throw PythonException();
    }
    return static_cast<int>(status);
  }

private:
  PyObject *owner_;
};

int MessageInit(PythonMessage *self, PyObject *args, PyObject *kwargs) {
  if (self->message) {
    PyErr_SetString(PyExc_RuntimeError, "Message is already initialized");
    return -1;
  }
  static const char *keywords[] = {nullptr};
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, ":Message",
                                   const_cast<char **>(keywords))) {
    return -1;
  }
  try {
    self->message = new lokiverse::Message;
    return 0;
  } catch (...) {
    SetCppException();
    return -1;
  }
}

void MessageDealloc(PythonMessage *self) {
  PyTypeObject *type = Py_TYPE(self);
  delete self->message;
  type->tp_free(reinterpret_cast<PyObject *>(self));
  Py_DECREF(type);
}

PyObject *MessageStream(PythonMessage *self, PyObject *) {
  if (!self->message) {
    PyErr_SetString(PyExc_RuntimeError, "Message is not initialized");
    return nullptr;
  }
  auto *state = State(Py_TYPE(self));
  if (!state || !state->stream_type) {
    if (!PyErr_Occurred()) {
      PyErr_SetString(PyExc_RuntimeError,
                      "lokiverse module is not initialized");
    }
    return nullptr;
  }
  PyRef result(state->stream_type->tp_alloc(state->stream_type, 0));
  if (!result.Get()) {
    return nullptr;
  }
  auto *stream = reinterpret_cast<PythonStream *>(result.Get());
  try {
    stream->owner = Py_NewRef(reinterpret_cast<PyObject *>(self));
    stream->builder =
        new lokiverse::Message::StreamBuilder(self->message->Stream());
    return Py_NewRef(result.Get());
  } catch (...) {
    SetCppException();
    return nullptr;
  }
}

PyObject *MessageEncode(PythonMessage *self, PyObject *) {
  if (!self->message) {
    PyErr_SetString(PyExc_RuntimeError, "Message is not initialized");
    return nullptr;
  }
  try {
    const auto snapshot = *self->message;
    std::optional<std::string> body;
    {
      AllowThreads threads;
      body = snapshot.Encode();
    }
    if (!body) {
      Py_RETURN_NONE;
    }
    return PyBytes_FromStringAndSize(body->data(), body->size());
  } catch (...) {
    SetCppException();
    return nullptr;
  }
}

int StreamTraverse(PythonStream *self, visitproc visit, void *arg) {
  Py_VISIT(Py_TYPE(self));
  Py_VISIT(self->owner);
  return 0;
}

int StreamClear(PythonStream *self) {
  delete std::exchange(self->builder, nullptr);
  Py_CLEAR(self->owner);
  return 0;
}

void StreamDealloc(PythonStream *self) {
  PyTypeObject *type = Py_TYPE(self);
  PyObject_GC_UnTrack(self);
  StreamClear(self);
  type->tp_free(reinterpret_cast<PyObject *>(self));
  Py_DECREF(type);
}

PyObject *StreamNew(PyTypeObject *, PyObject *, PyObject *) {
  PyErr_SetString(PyExc_TypeError,
                  "create a StreamBuilder with Message.stream()");
  return nullptr;
}

bool StreamReady(PythonStream *self) {
  if (!self->builder) {
    PyErr_SetString(PyExc_RuntimeError, "StreamBuilder is not initialized");
    return false;
  }
  return true;
}

PyObject *StreamReserve(PythonStream *self, PyObject *args, PyObject *kwargs) {
  if (!StreamReady(self)) {
    return nullptr;
  }
  static const char *keywords[] = {"label_count", "value_count", nullptr};
  PyObject *labels;
  PyObject *values;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO:reserve",
                                   const_cast<char **>(keywords), &labels,
                                   &values)) {
    return nullptr;
  }
  std::size_t label_count;
  std::size_t value_count;
  if (!Count(labels, label_count) || !Count(values, value_count)) {
    return nullptr;
  }
  try {
    self->builder->Reserve(label_count, value_count);
    return Py_NewRef(reinterpret_cast<PyObject *>(self));
  } catch (...) {
    SetCppException();
    return nullptr;
  }
}

PyObject *StreamLabel(PythonStream *self, PyObject *args, PyObject *kwargs) {
  if (!StreamReady(self)) {
    return nullptr;
  }
  static const char *keywords[] = {"name", "value", nullptr};
  PyObject *name_object;
  PyObject *value_object;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO:label",
                                   const_cast<char **>(keywords), &name_object,
                                   &value_object)) {
    return nullptr;
  }
  try {
    std::string name;
    std::string value;
    if (!CopyString(name_object, name, "name must be a str") ||
        !CopyString(value_object, value, "value must be a str")) {
      return nullptr;
    }
    self->builder->Label(std::move(name), std::move(value));
    return Py_NewRef(reinterpret_cast<PyObject *>(self));
  } catch (...) {
    SetCppException();
    return nullptr;
  }
}

PyObject *StreamEntry(PythonStream *self, PyObject *args, PyObject *kwargs) {
  if (!StreamReady(self)) {
    return nullptr;
  }
  static const char *keywords[] = {"timestamp_ns", "line", nullptr};
  PyObject *timestamp_object = nullptr;
  PyObject *line_object = nullptr;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|OO:entry",
                                   const_cast<char **>(keywords),
                                   &timestamp_object, &line_object)) {
    return nullptr;
  }
  if (!line_object && timestamp_object && PyUnicode_Check(timestamp_object) &&
      PyTuple_GET_SIZE(args) == 1) {
    line_object = timestamp_object;
    timestamp_object = nullptr;
  }
  if (!line_object) {
    PyErr_SetString(PyExc_TypeError,
                    "entry requires line or timestamp_ns and line");
    return nullptr;
  }
  try {
    std::uint64_t timestamp = 0;
    if (timestamp_object && !Timestamp(timestamp_object, timestamp)) {
      return nullptr;
    }
    std::string line;
    if (!CopyString(line_object, line, "line must be a str")) {
      return nullptr;
    }
    if (timestamp_object) {
      self->builder->Entry(timestamp, std::move(line));
    } else {
      self->builder->Entry(std::move(line));
    }
    return Py_NewRef(reinterpret_cast<PyObject *>(self));
  } catch (...) {
    SetCppException();
    return nullptr;
  }
}

PyObject *StreamCommit(PythonStream *self, PyObject *) {
  if (!StreamReady(self)) {
    return nullptr;
  }
  try {
    self->builder->Commit();
    Py_RETURN_NONE;
  } catch (...) {
    SetCppException();
    return nullptr;
  }
}

PyObject *AbstractNew(PyTypeObject *type, PyObject *, PyObject *) {
  auto *state = State(type);
  if (!state) {
    return nullptr;
  }
  if (type == state->transport_type || type == state->logger_type) {
    PyErr_SetString(PyExc_TypeError, "use a factory or implement a subclass");
    return nullptr;
  }
  return type->tp_alloc(type, 0);
}

PyObject *TransportMakeHttp(PyTypeObject *type, PyObject *args,
                            PyObject *kwargs) {
  auto *state = State(type);
  if (!state || !state->transport_type) {
    return nullptr;
  }
  static const char *keywords[] = {"host", "port", nullptr};
  PyObject *host_object;
  PyObject *port_object;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO:make_http",
                                   const_cast<char **>(keywords), &host_object,
                                   &port_object)) {
    return nullptr;
  }
  try {
    std::string host;
    if (!CopyString(host_object, host, "host must be a str")) {
      return nullptr;
    }
    if (host.empty() || host.find('\0') != std::string::npos) {
      PyErr_SetString(PyExc_ValueError,
                      "host must be nonempty and contain no nulls");
      return nullptr;
    }
    if (!PyLong_Check(port_object) || PyBool_Check(port_object)) {
      PyErr_SetString(PyExc_TypeError, "port must be an int");
      return nullptr;
    }
    const auto port = PyLong_AsLong(port_object);
    if (PyErr_Occurred() || port < 1 || port > 65535) {
      PyErr_Clear();
      PyErr_SetString(PyExc_ValueError, "port must be between 1 and 65535");
      return nullptr;
    }
    PyRef result(state->transport_type->tp_alloc(state->transport_type, 0));
    if (!result.Get()) {
      return nullptr;
    }
    reinterpret_cast<PythonTransport *>(result.Get())->transport =
        lokiverse::Transport::MakeHttp(host, static_cast<int>(port)).release();
    return Py_NewRef(result.Get());
  } catch (...) {
    SetCppException();
    return nullptr;
  }
}

int TransportTraverse(PythonTransport *self, visitproc visit, void *arg) {
  Py_VISIT(Py_TYPE(self));
  return 0;
}

void TransportDealloc(PythonTransport *self) {
  auto *type = Py_TYPE(self);
  PyObject_GC_UnTrack(self);
  delete self->transport;
  type->tp_free(reinterpret_cast<PyObject *>(self));
  Py_DECREF(type);
}

PyObject *TransportPost(PythonTransport *self, PyObject *args,
                        PyObject *kwargs) {
  if (!self->transport) {
    PyErr_SetString(PyExc_NotImplementedError, "implement Transport.post");
    return nullptr;
  }
  static const char *keywords[] = {"path", "body", "content_type", nullptr};
  PyObject *path_object;
  PyObject *body_object;
  PyObject *type_object;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OOO:post",
                                   const_cast<char **>(keywords), &path_object,
                                   &body_object, &type_object)) {
    return nullptr;
  }
  try {
    std::string path;
    std::string content_type;
    if (!CopyString(path_object, path, "path must be a str") ||
        !CopyString(type_object, content_type, "content_type must be a str")) {
      return nullptr;
    }
    if (!PyBytes_Check(body_object)) {
      PyErr_SetString(PyExc_TypeError, "body must be bytes");
      return nullptr;
    }
    const std::string_view body(PyBytes_AS_STRING(body_object),
                                PyBytes_GET_SIZE(body_object));
    int status;
    {
      AllowThreads threads;
      status = self->transport->Post(path, body, content_type);
    }
    return PyLong_FromLong(status);
  } catch (...) {
    SetCppException();
    return nullptr;
  }
}

PyObject *MakeLogger(PyTypeObject *type, PyObject *transport, bool safe) {
  auto *state = State(type);
  if (!state || !state->logger_type || !state->transport_type) {
    return nullptr;
  }
  if (!PyObject_TypeCheck(transport, state->transport_type)) {
    PyErr_SetString(PyExc_TypeError, "transport must be a lokiverse.Transport");
    return nullptr;
  }
  PyRef result(state->logger_type->tp_alloc(state->logger_type, 0));
  if (!result.Get()) {
    return nullptr;
  }
  auto *logger = reinterpret_cast<PythonLogger *>(result.Get());
  try {
    logger->owner = Py_NewRef(transport);
    auto native = std::make_unique<LoggerState>();
    auto *target = reinterpret_cast<PythonTransport *>(transport)->transport;
    if (!target) {
      native->adapter = std::make_unique<CallbackTransport>(transport);
      target = native->adapter.get();
    }
    native->logger = safe ? lokiverse::Logger::MakeSafe(*target)
                          : lokiverse::Logger::MakeUnsafe(*target);
    logger->state = native.release();
    return Py_NewRef(result.Get());
  } catch (...) {
    SetCppException();
    return nullptr;
  }
}

PyObject *LoggerMakeSafe(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
  static const char *keywords[] = {"transport", nullptr};
  PyObject *transport;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:make_safe",
                                   const_cast<char **>(keywords), &transport)) {
    return nullptr;
  }
  return MakeLogger(type, transport, true);
}

PyObject *LoggerMakeUnsafe(PyTypeObject *type, PyObject *args,
                           PyObject *kwargs) {
  static const char *keywords[] = {"transport", nullptr};
  PyObject *transport;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:make_unsafe",
                                   const_cast<char **>(keywords), &transport)) {
    return nullptr;
  }
  return MakeLogger(type, transport, false);
}

int LoggerTraverse(PythonLogger *self, visitproc visit, void *arg) {
  Py_VISIT(Py_TYPE(self));
  Py_VISIT(self->owner);
  return 0;
}

int LoggerClear(PythonLogger *self) {
  delete std::exchange(self->state, nullptr);
  Py_CLEAR(self->owner);
  return 0;
}

void LoggerDealloc(PythonLogger *self) {
  auto *type = Py_TYPE(self);
  PyObject_GC_UnTrack(self);
  LoggerClear(self);
  type->tp_free(reinterpret_cast<PyObject *>(self));
  Py_DECREF(type);
}

PyObject *LoggerPush(PythonLogger *self, PyObject *args, PyObject *kwargs) {
  if (!self->state) {
    PyErr_SetString(PyExc_NotImplementedError, "implement Logger.push");
    return nullptr;
  }
  static const char *keywords[] = {"message", nullptr};
  PyObject *object;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:push",
                                   const_cast<char **>(keywords), &object)) {
    return nullptr;
  }
  auto *state = State(Py_TYPE(self));
  if (!state || !state->message_type) {
    return nullptr;
  }
  if (!PyObject_TypeCheck(object, state->message_type)) {
    PyErr_SetString(PyExc_TypeError, "message must be a lokiverse.Message");
    return nullptr;
  }
  auto *message = reinterpret_cast<PythonMessage *>(object);
  if (!message->message) {
    PyErr_SetString(PyExc_RuntimeError, "Message is not initialized");
    return nullptr;
  }
  try {
    const auto snapshot = *message->message;
    bool success;
    {
      AllowThreads threads;
      success = self->state->logger->Push(snapshot);
    }
    return PyBool_FromLong(success);
  } catch (...) {
    SetCppException();
    return nullptr;
  }
}

int ValueInit(PythonValue *self, PyObject *args, PyObject *kwargs) {
  if (self->value) {
    PyErr_SetString(PyExc_RuntimeError, "Value is already initialized");
    return -1;
  }
  static const char *keywords[] = {"timestamp_ns", "line", nullptr};
  PyObject *timestamp;
  PyObject *line;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO:Value",
                                   const_cast<char **>(keywords), &timestamp,
                                   &line)) {
    return -1;
  }
  try {
    auto value = std::make_unique<lokiverse::Value>();
    if (!Timestamp(timestamp, value->timestamp_ns) ||
        !CopyString(line, value->line, "line must be a str")) {
      return -1;
    }
    self->value = value.release();
    return 0;
  } catch (...) {
    SetCppException();
    return -1;
  }
}

void ValueDealloc(PythonValue *self) {
  auto *type = Py_TYPE(self);
  delete self->value;
  type->tp_free(reinterpret_cast<PyObject *>(self));
  Py_DECREF(type);
}

PyObject *ValueNow(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
  static const char *keywords[] = {"line", nullptr};
  PyObject *line;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:now",
                                   const_cast<char **>(keywords), &line)) {
    return nullptr;
  }
  try {
    std::string text;
    if (!CopyString(line, text, "line must be a str")) {
      return nullptr;
    }
    PyRef result(type->tp_alloc(type, 0));
    if (!result.Get()) {
      return nullptr;
    }
    reinterpret_cast<PythonValue *>(result.Get())->value =
        new lokiverse::Value(lokiverse::Value::Now(std::move(text)));
    return Py_NewRef(result.Get());
  } catch (...) {
    SetCppException();
    return nullptr;
  }
}

PyObject *ValueTimestamp(PythonValue *self, void *) {
  if (!self->value) {
    PyErr_SetString(PyExc_RuntimeError, "Value is not initialized");
    return nullptr;
  }
  return PyLong_FromUnsignedLongLong(self->value->timestamp_ns);
}

int ValueSetTimestamp(PythonValue *self, PyObject *value, void *) {
  if (!self->value || !value) {
    PyErr_SetString(
        PyExc_AttributeError,
        "timestamp_ns cannot be deleted or set before initialization");
    return -1;
  }
  std::uint64_t timestamp;
  if (!Timestamp(value, timestamp)) {
    return -1;
  }
  self->value->timestamp_ns = timestamp;
  return 0;
}

PyObject *ValueLine(PythonValue *self, void *) {
  if (!self->value) {
    PyErr_SetString(PyExc_RuntimeError, "Value is not initialized");
    return nullptr;
  }
  return PyUnicode_DecodeUTF8(self->value->line.data(),
                              self->value->line.size(), "strict");
}

int ValueSetLine(PythonValue *self, PyObject *value, void *) {
  if (!self->value || !value) {
    PyErr_SetString(PyExc_AttributeError,
                    "line cannot be deleted or set before initialization");
    return -1;
  }
  try {
    std::string line;
    if (!CopyString(value, line, "line must be a str")) {
      return -1;
    }
    self->value->line = std::move(line);
    return 0;
  } catch (...) {
    SetCppException();
    return -1;
  }
}

PyDoc_STRVAR(MessageDoc,
             "Message()\n--\n\nOwn labeled streams of log entries.");
PyDoc_STRVAR(StreamDoc, "Build one stream through Message.stream().");
PyDoc_STRVAR(
    TransportDoc,
    "Synchronous transport interface. Implement post or use make_http.");
PyDoc_STRVAR(
    LoggerDoc,
    "Logger interface. Implement push or use make_safe or make_unsafe.");
PyDoc_STRVAR(ValueDoc, "Value(timestamp_ns, line)\n--\n\nOwn a Unix nanosecond "
                       "timestamp and line.");
PyDoc_STRVAR(StreamMethodDoc,
             "stream($self, /)\n--\n\nCreate an uncommitted stream builder.");
PyDoc_STRVAR(EncodeDoc,
             "encode($self, /)\n--\n\nReturn raw-Snappy-compressed protobuf "
             "bytes, or None for an empty or unserializable message.");
PyDoc_STRVAR(ReserveDoc,
             "reserve($self, /, label_count, value_count)\n--\n\nReserve total "
             "label and entry capacities for this stream and return self.");
PyDoc_STRVAR(LabelDoc, "label($self, /, name, value)\n--\n\nCopy one label and "
                       "return self. Duplicate names are preserved.");
PyDoc_STRVAR(EntryDoc,
             "entry($self, /, *args, **kwargs)\n--\n\nCopy entry(line) using "
             "the current time, or entry(timestamp_ns, line) with explicit "
             "Unix nanoseconds. Return self.");
PyDoc_STRVAR(CommitDoc, "commit($self, /)\n--\n\nAppend the stream and reset "
                        "the builder. Raise ValueError without entries.");
PyDoc_STRVAR(HttpDoc, "make_http($type, /, host, port)\n--\n\nCreate an HTTP "
                      "transport for a hostname and port.");
PyDoc_STRVAR(PostDoc,
             "post($self, /, path, body, content_type)\n--\n\nSynchronously "
             "post bytes. Return an HTTP status or -1 on transport failure.");
PyDoc_STRVAR(
    SafeDoc,
    "make_safe($type, /, transport)\n--\n\nCreate a logger serializing "
    "transport calls. Do not use the transport concurrently elsewhere.");
PyDoc_STRVAR(UnsafeDoc,
             "make_unsafe($type, /, transport)\n--\n\nCreate a logger without "
             "internal locking. Callers synchronize transport access.");
PyDoc_STRVAR(
    PushDoc,
    "push($self, /, message)\n--\n\nPush a snapshot. Return True only for HTTP "
    "204; empty messages return False. Release the GIL during native work.");
PyDoc_STRVAR(NowDoc, "now($type, /, line)\n--\n\nCapture the current Unix "
                     "nanosecond timestamp and own line.");

PyMethodDef MessageMethods[] = {
    {"stream", reinterpret_cast<PyCFunction>(MessageStream), METH_NOARGS,
     StreamMethodDoc},
    {"encode", reinterpret_cast<PyCFunction>(MessageEncode), METH_NOARGS,
     EncodeDoc},
    {nullptr, nullptr, 0, nullptr},
};
PyMethodDef StreamMethods[] = {
    {"reserve", reinterpret_cast<PyCFunction>(StreamReserve),
     METH_VARARGS | METH_KEYWORDS, ReserveDoc},
    {"label", reinterpret_cast<PyCFunction>(StreamLabel),
     METH_VARARGS | METH_KEYWORDS, LabelDoc},
    {"entry", reinterpret_cast<PyCFunction>(StreamEntry),
     METH_VARARGS | METH_KEYWORDS, EntryDoc},
    {"commit", reinterpret_cast<PyCFunction>(StreamCommit), METH_NOARGS,
     CommitDoc},
    {nullptr, nullptr, 0, nullptr},
};
PyMethodDef TransportMethods[] = {
    {"make_http", reinterpret_cast<PyCFunction>(TransportMakeHttp),
     METH_CLASS | METH_VARARGS | METH_KEYWORDS, HttpDoc},
    {"post", reinterpret_cast<PyCFunction>(TransportPost),
     METH_VARARGS | METH_KEYWORDS, PostDoc},
    {nullptr, nullptr, 0, nullptr},
};
PyMethodDef LoggerMethods[] = {
    {"make_safe", reinterpret_cast<PyCFunction>(LoggerMakeSafe),
     METH_CLASS | METH_VARARGS | METH_KEYWORDS, SafeDoc},
    {"make_unsafe", reinterpret_cast<PyCFunction>(LoggerMakeUnsafe),
     METH_CLASS | METH_VARARGS | METH_KEYWORDS, UnsafeDoc},
    {"push", reinterpret_cast<PyCFunction>(LoggerPush),
     METH_VARARGS | METH_KEYWORDS, PushDoc},
    {nullptr, nullptr, 0, nullptr},
};
PyMethodDef ValueMethods[] = {
    {"now", reinterpret_cast<PyCFunction>(ValueNow),
     METH_CLASS | METH_VARARGS | METH_KEYWORDS, NowDoc},
    {nullptr, nullptr, 0, nullptr},
};
PyGetSetDef ValueFields[] = {
    {"timestamp_ns", reinterpret_cast<getter>(ValueTimestamp),
     reinterpret_cast<setter>(ValueSetTimestamp), "Unix epoch nanoseconds",
     nullptr},
    {"line", reinterpret_cast<getter>(ValueLine),
     reinterpret_cast<setter>(ValueSetLine), "Owned log line", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

PyType_Slot MessageSlots[] = {
    {Py_tp_doc, const_cast<char *>(MessageDoc)},
    {Py_tp_new, reinterpret_cast<void *>(PyType_GenericNew)},
    {Py_tp_init, reinterpret_cast<void *>(MessageInit)},
    {Py_tp_dealloc, reinterpret_cast<void *>(MessageDealloc)},
    {Py_tp_methods, MessageMethods},
    {0, nullptr},
};
PyType_Slot StreamSlots[] = {
    {Py_tp_doc, const_cast<char *>(StreamDoc)},
    {Py_tp_new, reinterpret_cast<void *>(StreamNew)},
    {Py_tp_dealloc, reinterpret_cast<void *>(StreamDealloc)},
    {Py_tp_traverse, reinterpret_cast<void *>(StreamTraverse)},
    {Py_tp_clear, reinterpret_cast<void *>(StreamClear)},
    {Py_tp_methods, StreamMethods},
    {0, nullptr},
};
PyType_Slot TransportSlots[] = {
    {Py_tp_doc, const_cast<char *>(TransportDoc)},
    {Py_tp_new, reinterpret_cast<void *>(AbstractNew)},
    {Py_tp_dealloc, reinterpret_cast<void *>(TransportDealloc)},
    {Py_tp_traverse, reinterpret_cast<void *>(TransportTraverse)},
    {Py_tp_methods, TransportMethods},
    {0, nullptr},
};
PyType_Slot LoggerSlots[] = {
    {Py_tp_doc, const_cast<char *>(LoggerDoc)},
    {Py_tp_new, reinterpret_cast<void *>(AbstractNew)},
    {Py_tp_dealloc, reinterpret_cast<void *>(LoggerDealloc)},
    {Py_tp_traverse, reinterpret_cast<void *>(LoggerTraverse)},
    {Py_tp_clear, reinterpret_cast<void *>(LoggerClear)},
    {Py_tp_methods, LoggerMethods},
    {0, nullptr},
};
PyType_Slot ValueSlots[] = {
    {Py_tp_doc, const_cast<char *>(ValueDoc)},
    {Py_tp_new, reinterpret_cast<void *>(PyType_GenericNew)},
    {Py_tp_init, reinterpret_cast<void *>(ValueInit)},
    {Py_tp_dealloc, reinterpret_cast<void *>(ValueDealloc)},
    {Py_tp_methods, ValueMethods},
    {Py_tp_getset, ValueFields},
    {0, nullptr},
};

PyType_Spec MessageSpec = {"lokiverse.Message", sizeof(PythonMessage), 0,
                           Py_TPFLAGS_DEFAULT, MessageSlots};
PyType_Spec StreamSpec = {"lokiverse.StreamBuilder", sizeof(PythonStream), 0,
                          Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, StreamSlots};
PyType_Spec TransportSpec = {"lokiverse.Transport", sizeof(PythonTransport), 0,
                             Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
                                 Py_TPFLAGS_BASETYPE,
                             TransportSlots};
PyType_Spec LoggerSpec = {
    "lokiverse.Logger", sizeof(PythonLogger), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE, LoggerSlots};
PyType_Spec ValueSpec = {"lokiverse.Value", sizeof(PythonValue), 0,
                         Py_TPFLAGS_DEFAULT, ValueSlots};

bool AddType(PyObject *module, const char *name, PyType_Spec &spec,
             PyTypeObject *&target) {
  PyRef type(PyType_FromModuleAndSpec(module, &spec, nullptr));
  if (!type.Get()) {
    return false;
  }
  Py_XSETREF(target, reinterpret_cast<PyTypeObject *>(Py_NewRef(type.Get())));
  return PyModule_AddObjectRef(module, name, type.Get()) == 0;
}

int ModuleExec(PyObject *module) {
  auto *state = static_cast<ModuleState *>(PyModule_GetState(module));
  if (!AddType(module, "Message", MessageSpec, state->message_type) ||
      !AddType(module, "StreamBuilder", StreamSpec, state->stream_type) ||
      !AddType(module, "Transport", TransportSpec, state->transport_type) ||
      !AddType(module, "Logger", LoggerSpec, state->logger_type) ||
      !AddType(module, "Value", ValueSpec, state->value_type)) {
    return -1;
  }
  return PyModule_AddObjectRef(module, "Labels",
                               reinterpret_cast<PyObject *>(&PyList_Type));
}

int ModuleTraverse(PyObject *module, visitproc visit, void *arg) {
  auto *state = static_cast<ModuleState *>(PyModule_GetState(module));
  if (state) {
    Py_VISIT(state->message_type);
    Py_VISIT(state->stream_type);
    Py_VISIT(state->transport_type);
    Py_VISIT(state->logger_type);
    Py_VISIT(state->value_type);
  }
  return 0;
}

int ModuleClear(PyObject *module) {
  auto *state = static_cast<ModuleState *>(PyModule_GetState(module));
  if (state) {
    Py_CLEAR(state->message_type);
    Py_CLEAR(state->stream_type);
    Py_CLEAR(state->transport_type);
    Py_CLEAR(state->logger_type);
    Py_CLEAR(state->value_type);
  }
  return 0;
}

void ModuleFree(void *module) { ModuleClear(static_cast<PyObject *>(module)); }

PyModuleDef_Slot ModuleSlots[] = {
    {Py_mod_exec, reinterpret_cast<void *>(ModuleExec)},
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_USED},
    {0, nullptr},
};
PyModuleDef Module = {
    PyModuleDef_HEAD_INIT, "lokiverse", "Push log streams to Loki.",
    sizeof(ModuleState),   nullptr,     ModuleSlots,
    ModuleTraverse,        ModuleClear, ModuleFree,
};

}

PyMODINIT_FUNC PyInit_lokiverse() { return PyModuleDef_Init(&Module); }
