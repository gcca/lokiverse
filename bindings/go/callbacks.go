package lokiverse

/*
#include <stdint.h>
#include "lokiverse.h"
int lokiverse_go_transport(uintptr_t handle, lokiverse_transport **transport);
*/
import "C"

import (
	"runtime/cgo"
	"unsafe"
)

func customTransport(transport Transport) (*C.lokiverse_transport, error) {
	handle := cgo.NewHandle(transport)
	var native *C.lokiverse_transport
	if err := statusError(C.lokiverse_go_transport(C.uintptr_t(handle), &native)); err != nil {
		handle.Delete()
		return nil, err
	}
	return native, nil
}

//export lokiverseGoPost
func lokiverseGoPost(handle C.uintptr_t, path *C.char, pathSize C.size_t, body *C.char, bodySize C.size_t, contentType *C.char, contentTypeSize C.size_t) (status C.int) {
	status = -1
	defer func() {
		if recover() != nil {
			status = -1
		}
	}()
	bytes := func(data *C.char, size C.size_t) []byte {
		return unsafe.Slice((*byte)(unsafe.Pointer(data)), int(size))
	}
	transport := cgo.Handle(handle).Value().(Transport)
	result := transport.Post(string(bytes(path, pathSize)), append([]byte(nil), bytes(body, bodySize)...), string(bytes(contentType, contentTypeSize)))
	if result < -2147483648 || result > 2147483647 {
		return -1
	}
	return C.int(result)
}

//export lokiverseGoDestroy
func lokiverseGoDestroy(handle C.uintptr_t) { cgo.Handle(handle).Delete() }
