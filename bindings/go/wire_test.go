package lokiverse

import (
	"encoding/binary"
	"errors"
)

type wireEntry struct {
	timestamp    uint64
	line         string
	hasTimestamp bool
	nanos        uint64
}

type wireStream struct {
	labels  string
	entries []wireEntry
}

var errWire = errors.New("malformed wire data")

func uvarint(data []byte) (uint64, []byte, error) {
	value, size := binary.Uvarint(data)
	if size <= 0 {
		return 0, nil, errWire
	}
	return value, data[size:], nil
}

func unsnappy(data []byte) ([]byte, error) {
	total, data, err := uvarint(data)
	if err != nil {
		return nil, err
	}
	output := make([]byte, 0, total)
	for len(data) > 0 {
		tag := data[0]
		data = data[1:]
		var length, offset, extra int
		switch tag & 3 {
		case 0:
			length = int(tag>>2) + 1
			if length > 60 {
				extra = length - 60
				if len(data) < extra {
					return nil, errWire
				}
				length = 0
				for i := extra - 1; i >= 0; i-- {
					length = length<<8 | int(data[i])
				}
				length++
				data = data[extra:]
			}
			if len(data) < length {
				return nil, errWire
			}
			output = append(output, data[:length]...)
			data = data[length:]
			continue
		case 1:
			extra = 1
			length = 4 + int(tag>>2&7)
			offset = int(tag>>5) << 8
		case 2:
			extra = 2
			length = int(tag>>2) + 1
		case 3:
			extra = 4
			length = int(tag>>2) + 1
		}
		if len(data) < extra {
			return nil, errWire
		}
		low := 0
		for i := extra - 1; i >= 0; i-- {
			low = low<<8 | int(data[i])
		}
		offset |= low
		data = data[extra:]
		if offset == 0 || offset > len(output) {
			return nil, errWire
		}
		for i := 0; i < length; i++ {
			output = append(output, output[len(output)-offset])
		}
	}
	if uint64(len(output)) != total {
		return nil, errWire
	}
	return output, nil
}

func fields(data []byte, visit func(number uint64, value uint64, bytes []byte) error) error {
	for len(data) > 0 {
		key, rest, err := uvarint(data)
		if err != nil {
			return err
		}
		data = rest
		switch key & 7 {
		case 0:
			value, rest, err := uvarint(data)
			if err != nil {
				return err
			}
			data = rest
			if err := visit(key>>3, value, nil); err != nil {
				return err
			}
		case 2:
			size, rest, err := uvarint(data)
			if err != nil || uint64(len(rest)) < size {
				return errWire
			}
			if err := visit(key>>3, 0, rest[:size]); err != nil {
				return err
			}
			data = rest[size:]
		default:
			return errWire
		}
	}
	return nil
}

func decodeWire(body []byte) ([]wireStream, error) {
	raw, err := unsnappy(body)
	if err != nil {
		return nil, err
	}
	var streams []wireStream
	err = fields(raw, func(number, _ uint64, bytes []byte) error {
		if number != 1 || bytes == nil {
			return errWire
		}
		var stream wireStream
		err := fields(bytes, func(number, _ uint64, bytes []byte) error {
			switch {
			case number == 1 && bytes != nil:
				stream.labels = string(bytes)
			case number == 2 && bytes != nil:
				var item wireEntry
				err := fields(bytes, func(number, _ uint64, bytes []byte) error {
					switch {
					case number == 1 && bytes != nil:
						item.hasTimestamp = true
						var seconds uint64
						err := fields(bytes, func(number, value uint64, bytes []byte) error {
							switch {
							case number == 1 && bytes == nil:
								seconds = value
							case number == 2 && bytes == nil:
								item.nanos = value
							default:
								return errWire
							}
							return nil
						})
						item.timestamp = seconds*1000000000 + item.nanos
						return err
					case number == 2 && bytes != nil:
						item.line = string(bytes)
					default:
						return errWire
					}
					return nil
				})
				if err != nil {
					return err
				}
				stream.entries = append(stream.entries, item)
			default:
				return errWire
			}
			return nil
		})
		streams = append(streams, stream)
		return err
	})
	return streams, err
}
