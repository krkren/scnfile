# The PSB format, as scnfile reads and writes it

PSB is M2's binary format for a tree of values. This is the layout `plugins/scnfile/psb.cpp`
implements; `tests/psb_test.cpp` holds files written by hand from this page. It was written from
the format, not from other implementations' code.

All numbers are little-endian. Offsets are from the start of the file.

**Compatibility.** scnfile reads and writes its own files. Whether it reads every PSB Wamsoft's
tools write, and whether their `PSBFile` reads scnfile's, hasn't been checked against real files
from them: the layout here is believed to be the common one, and the reader refuses what it
doesn't know rather than guessing.

## Header

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | `PSB\0` |
| 0x04 | 2 | version: 2, 3 or 4 are read; scnfile writes 2 |
| 0x06 | 2 | header encryption: 0. Anything else is refused (scnfile doesn't decrypt M2's encryption) |
| 0x08 | 4 | where encryption would start; scnfile writes the header's size |
| 0x0C | 4 | names |
| 0x10 | 4 | string offsets |
| 0x14 | 4 | string data |
| 0x18 | 4 | resource offsets |
| 0x1C | 4 | resource lengths |
| 0x20 | 4 | resource data |
| 0x24 | 4 | the root value |
| 0x28 | 4 | version 3 and 4: a checksum (not checked) |
| 0x2C | 4 | version 4: extra resource offsets (0: none) |
| 0x30 | 4 | version 4: extra resource lengths |
| 0x34 | 4 | version 4: extra resource data |

The header is 0x28 bytes in version 2, 0x2C in version 3 and 0x38 in version 4. The sections
can be in any order; scnfile writes header, names, root, string offsets, strings, resource
offsets, resource lengths, resources.

## Integer arrays

The tables are integer arrays:

| Bytes | |
|---|---|
| 1 | 0x0C + *c*: the count takes *c* bytes (1–8) |
| *c* | the count *n* |
| 1 | 0x0C + *e*: each entry takes *e* bytes (1–8) |
| *n* × *e* | the entries, unsigned |

The entry size byte is there even when the count is 0. scnfile writes the fewest bytes that hold
the count and the largest entry.

## Names

Object member names, at the names offset: three integer arrays, *base*, *check* and *ends*,
which make a double-array trie over the names' UTF-8 bytes. Node 0 is the root. A node's child
for byte *c* is node *base*[node] + *c*, and *check*[child] is the node. A name ends with byte 0:
*ends*[*i*] is the node that byte leads to for name *i*.

To read name *i*: start at *node* = *check*[*ends*[*i*]] (the node of its last byte). While
*node* isn't 0, its byte is *node* − *base*[*check*[*node*]]; go on with *node* =
*check*[*node*]. The bytes come out last first.

scnfile sorts the names by their bytes before numbering them, and builds the trie breadth first,
giving each node the smallest base that puts all its children in free nodes past the root.

## Strings and resources

Strings: an integer array of offsets into the string data, where each string is UTF-8 ending
with a 0 byte. A string used more than once is stored once.

Resources (binary data): an integer array of offsets into the resource data and one of lengths,
with as many entries. Identical resources are stored once. Version 4's extra resources are a
second set of the same.

## Values

The first byte of a value says what it is:

| Byte | Value | What follows |
|---|---|---|
| 0x01 | null | |
| 0x02, 0x03 | false, true | |
| 0x04 | the integer 0 | |
| 0x05–0x0C | an integer | 1–8 bytes (byte − 0x04), signed |
| 0x0D–0x14 | an integer array (above), as a value | the rest of the array; it reads as a list of integers |
| 0x15–0x18 | a string | its index, 1–4 bytes (byte − 0x14) |
| 0x19–0x1C | a resource | its index, 1–4 bytes (byte − 0x18) |
| 0x1D | the real 0.0 | |
| 0x1E | a real | a 4-byte IEEE float |
| 0x1F | a real | an 8-byte IEEE double |
| 0x20 | a list | an integer array of offsets, then the items |
| 0x21 | an object | an integer array of name indexes, one of offsets, then the members |
| 0x22–0x25 | a version 4 extra resource | its index, 1–4 bytes (byte − 0x21) |

Any other byte is refused.

A list's or an object's offsets are from the end of its last integer array, where its items
start. An object's name indexes are in ascending order, so its members are sorted by name.

scnfile writes integers and indexes in the fewest bytes that hold them, +0.0 as 0x1D, a real
that a float holds exactly as 0x1E and any other as 0x1F. It doesn't write integer arrays as
values, booleans (TJS has none) or version 4's extra resources.

## mdf

A file can be zlib-compressed: `mdf\0`, the PSB's size (4 bytes), then the zlib stream. scnfile
reads these when it's built with zlib, and doesn't write them.

## Limits

The reader refuses a file it would read out of bounds, values nested over 1000 deep, and more
than 50 million values (a file whose offsets point back at shared values could otherwise expand
without end). Files are under 4 GB.

## The TJS mapping (SCNFile)

| TJS | PSB |
|---|---|
| void | null |
| Integer | integer |
| Real | real |
| String | string (UTF-8; invalid UTF-8 reads as U+FFFD) |
| Octet | resource |
| Array | list |
| Dictionary | object |

A PSB boolean reads as the Integer 1 or 0. Other objects can't be written, nor a Dictionary
that holds itself.
