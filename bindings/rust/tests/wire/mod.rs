#[derive(Debug, Default, PartialEq)]
pub struct Entry {
    pub timestamp: u64,
    pub nanos: u64,
    pub has_timestamp: bool,
    pub line: Vec<u8>,
}

#[derive(Debug, Default, PartialEq)]
pub struct Stream {
    pub labels: String,
    pub entries: Vec<Entry>,
}

impl Stream {
    pub fn values(&self) -> Vec<(u64, &[u8])> {
        self.entries
            .iter()
            .map(|entry| (entry.timestamp, entry.line.as_slice()))
            .collect()
    }
}

fn varint(data: &mut &[u8]) -> u64 {
    let mut value = 0u64;
    let mut shift = 0;
    loop {
        let byte = data[0];
        *data = &data[1..];
        value |= u64::from(byte & 0x7f) << shift;
        if byte & 0x80 == 0 {
            return value;
        }
        shift += 7;
        assert!(shift < 64, "varint too long");
    }
}

fn little_endian(data: &mut &[u8], count: usize) -> usize {
    let mut value = 0usize;
    for index in (0..count).rev() {
        value = value << 8 | usize::from(data[index]);
    }
    *data = &data[count..];
    value
}

pub fn unsnappy(mut data: &[u8]) -> Vec<u8> {
    let total = varint(&mut data) as usize;
    let mut output = Vec::with_capacity(total);
    while !data.is_empty() {
        let tag = data[0];
        data = &data[1..];
        let upper = usize::from(tag >> 2);
        let (length, offset) = match tag & 3 {
            0 => {
                let length = if upper < 60 {
                    upper + 1
                } else {
                    little_endian(&mut data, upper - 59) + 1
                };
                output.extend_from_slice(&data[..length]);
                data = &data[length..];
                continue;
            }
            1 => (
                4 + (upper & 7),
                usize::from(tag >> 5) << 8 | little_endian(&mut data, 1),
            ),
            2 => (upper + 1, little_endian(&mut data, 2)),
            _ => (upper + 1, little_endian(&mut data, 4)),
        };
        assert!(offset != 0 && offset <= output.len(), "bad copy offset");
        for _ in 0..length {
            output.push(output[output.len() - offset]);
        }
    }
    assert_eq!(output.len(), total);
    output
}

enum Field<'a> {
    Number(u64),
    Bytes(&'a [u8]),
}

fn fields(mut data: &[u8]) -> Vec<(u64, Field<'_>)> {
    let mut result = Vec::new();
    while !data.is_empty() {
        let key = varint(&mut data);
        let field = match key & 7 {
            0 => Field::Number(varint(&mut data)),
            2 => {
                let size = varint(&mut data) as usize;
                let (bytes, rest) = data.split_at(size);
                data = rest;
                Field::Bytes(bytes)
            }
            kind => panic!("unexpected wire type {kind}"),
        };
        result.push((key >> 3, field));
    }
    result
}

pub fn decode(body: &[u8]) -> Vec<Stream> {
    let raw = unsnappy(body);
    let mut streams = Vec::new();
    for field in fields(&raw) {
        let (1, Field::Bytes(bytes)) = field else {
            panic!("unexpected PushRequest field");
        };
        let mut stream = Stream::default();
        for field in fields(bytes) {
            match field {
                (1, Field::Bytes(bytes)) => {
                    stream.labels = String::from_utf8(bytes.to_vec()).unwrap()
                }
                (2, Field::Bytes(bytes)) => {
                    let mut entry = Entry::default();
                    for field in fields(bytes) {
                        match field {
                            (1, Field::Bytes(bytes)) => {
                                entry.has_timestamp = true;
                                let mut seconds = 0;
                                for field in fields(bytes) {
                                    match field {
                                        (1, Field::Number(value)) => seconds = value,
                                        (2, Field::Number(value)) => entry.nanos = value,
                                        _ => panic!("unexpected Timestamp field"),
                                    }
                                }
                                entry.timestamp = seconds * 1_000_000_000 + entry.nanos;
                            }
                            (2, Field::Bytes(bytes)) => entry.line = bytes.to_vec(),
                            _ => panic!("unexpected Entry field"),
                        }
                    }
                    stream.entries.push(entry);
                }
                _ => panic!("unexpected Stream field"),
            }
        }
        streams.push(stream);
    }
    streams
}
