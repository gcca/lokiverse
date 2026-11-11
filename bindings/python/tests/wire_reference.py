import snappy
from google.protobuf import descriptor_pb2, descriptor_pool, message_factory, timestamp_pb2


def request_type():
    file = descriptor_pb2.FileDescriptorProto(
        name="loki_reference.proto",
        package="logproto",
        syntax="proto3",
        dependency=["google/protobuf/timestamp.proto"],
    )
    definitions = (
        ("EntryAdapter", (
            ("timestamp", 1, 11, 1, ".google.protobuf.Timestamp"),
            ("line", 2, 9, 1, ""),
        )),
        ("StreamAdapter", (
            ("labels", 1, 9, 1, ""),
            ("entries", 2, 11, 3, ".logproto.EntryAdapter"),
        )),
        ("PushRequest", (
            ("streams", 1, 11, 3, ".logproto.StreamAdapter"),
        )),
    )
    for name, fields in definitions:
        message = file.message_type.add(name=name)
        for field_name, number, field_type, label, type_name in fields:
            field = message.field.add(
                name=field_name, number=number, type=field_type, label=label
            )
            if type_name:
                field.type_name = type_name
    pool = descriptor_pool.DescriptorPool()
    pool.AddSerializedFile(timestamp_pb2.DESCRIPTOR.serialized_pb)
    descriptor = pool.Add(file).message_types_by_name["PushRequest"]
    return message_factory.GetMessageClass(descriptor)


PushRequest = request_type()


def decode(body):
    return PushRequest.FromString(snappy.decompress(body))


def values(stream):
    return [
        (entry.timestamp.seconds * 1_000_000_000 + entry.timestamp.nanos, entry.line)
        for entry in stream.entries
    ]
