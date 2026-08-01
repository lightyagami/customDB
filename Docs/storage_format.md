# Storage Format & Packed Records

This document details the on-disk storage layout of our database engine, covering slotted page leaf nodes, internal nodes, and the SQLite-style packed record format.

---

## Page Layouts

The database file consists of a sequence of $4\,\text{KB}$ ($4096\,\text{bytes}$) pages. Every page starts with a header that indicates its node type:

```
[Node Type (1 byte)] [Is Root (1 byte)] [Header Payload...]
```

### 1. Internal Nodes (Index Routing Pages)
Internal nodes guide B+ Tree traversals and contain an ordered list of keys and child page pointers:
- **Header**:
  - `Node Type`: `NODE_INTERNAL` (`0x02`)
  - `Is Root`: `1` or `0`
  - `Num Keys` (`uint32_t`): Count of keys present in this node.
  - `Right Child` (`uint32_t`): Page pointer to the rightmost child.
- **Cells**:
  - Array of child pointers: `Child 0`, `Child 1`, ..., `Child N`.
  - Array of routing keys: `Key 0`, `Key 1`, ..., `Key N-1` (fixed $32\,\text{bytes}$ per key).

---

### 2. Leaf Nodes (Slotted Page Storage)
Leaf nodes store the actual serialized row records. To support variable-length records without fragmentation, leaf pages use a **slotted page layout**:

```
+-----------------------------------------------------------------------+
| Header | Slot 0 | Slot 1 | ... |              <-- Free Space -->      |
+-----------------------------------------------------------------------+
|                                    ... | Record 1 | Record 0 |        |
+-----------------------------------------------------------------------+
```

- **Header**:
  - `Node Type`: `NODE_LEAF` (`0x01`)
  - `Is Root`: `1` or `0`
  - `Num Cells` (`uint16_t`): Number of rows on this page.
  - `Next Leaf` (`uint32_t`): Page number of the sibling leaf (enabling fast range scans).
  - `Free Space Offset` (`uint16_t`): Pointer to the beginning of the free space area.
- **Slots**:
  - Array of `PageSlot` structs: `Slot 0`, `Slot 1`, ..., `Slot N-1`.
  - Each slot contains `[offset: uint16_t] [size: uint16_t]`.
- **Cell Records**:
  - Written from the end of the page moving backwards.
  - Defragmented automatically when free space is fragmented.

---

## SQLite-style Packed Record Format

Row values are packed into a compact byte stream inside each cell using the SQLite packed record layout:

```
[Header Size (varint)] [Serial Type Code 0 (varint)] ... [Serial Type Code N] [Data Payload Bytes]
```

### 1. Variable-Length Integers (Varints)
Varints use a base-128 (protobuf-style) variable byte encoding. The high-order bit of each byte is set if more bytes follow; the final byte has the high bit cleared:
- A value $< 128$ takes $1\,\text{byte}$.
- Large values take up to $9\,\text{bytes}$.

### 2. Serial Type Code Reference
The serial type code determines the type of the column value and its data size in the payload:

| Serial Type | Data Size | Data Type / Meaning |
|---|---|---|
| **`0`** | 0 bytes | NULL |
| **`1`** | 1 byte | 8-bit signed integer |
| **`2`** | 2 bytes | 16-bit signed integer |
| **`4`** | 4 bytes | 32-bit signed integer |
| **`7`** | 8 bytes | 64-bit IEEE 754 float / double |
| **`8`** | 0 bytes | Constant value `0` (no payload) |
| **`9`** | 0 bytes | Constant value `1` (no payload) |
| **`13 + 2 * N`** | N bytes | TEXT string of length `N` |

### 3. Leaf Key Extraction
Because keys (Column 0) are packed dynamically inside the record, B+ Tree search operations (`btree_find`) call:
```c
extract_col0_from_packed_record(TableDef* def, const void* record_bytes, Value* out_val);
```
This extracts Column 0 from the start of the record payload in $O(1)$ time, enabling fast binary search comparisons without full row deserialization.
