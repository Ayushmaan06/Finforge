"""File storage at the trust boundary (PLAN Phase 7 task 5).

Uploads are streamed to disk with a size cap, their type is sniffed from the content (never from
the client's filename or Content-Type), and they are stored under a server-generated name. The
client's filename is kept only as sanitised metadata.
"""

import hashlib
import re
import uuid
from pathlib import Path
from typing import BinaryIO

EXTENSION = {"csv": ".csv", "zip": ".zip", "parquet": ".parquet", "json": ".json"}
_SAFE = re.compile(r"[^A-Za-z0-9._-]+")


class UploadTooLarge(Exception):
    pass


class UnsupportedContent(Exception):
    pass


def sanitise_filename(name: str | None) -> str:
    base = Path(name or "upload").name  # drops any directory part, including "../"
    base = _SAFE.sub("_", base).strip("._") or "upload"
    return base[:120]


def sniff(head: bytes, tail: bytes) -> str:
    """Content type from the first and last bytes: zip, parquet, json or csv (text)."""
    if head.startswith(b"PK\x03\x04"):
        return "zip"
    if head.startswith(b"PAR1") and tail.endswith(b"PAR1"):
        return "parquet"
    if b"\x00" in head:
        raise UnsupportedContent("binary content that is not zip or parquet")
    try:
        text = head.decode("utf-8")
    except UnicodeDecodeError:
        # The head may end inside a multi-byte character; retry without the last 3 bytes.
        try:
            text = head[:-3].decode("utf-8")
        except UnicodeDecodeError:
            raise UnsupportedContent("not UTF-8 text") from None
    stripped = text.lstrip("﻿ \t\r\n")
    if stripped.startswith(("[", "{")):
        return "json"
    first_line = stripped.split("\n", 1)[0]
    if "," in first_line:
        return "csv"
    raise UnsupportedContent("not CSV (no comma-separated first line), JSON, zip or parquet")


def save_upload(
    stream: BinaryIO, directory: Path, max_bytes: int
) -> tuple[str, Path, int, str, str]:
    """Stream to a temporary file, enforcing the cap; sniff; move to `<uuid><ext>`.
    Returns (id, path, size, sha256, content_type)."""
    directory.mkdir(parents=True, exist_ok=True)
    upload_id = str(uuid.uuid4())
    tmp = directory / f".{upload_id}.part"
    h = hashlib.sha256()
    size = 0
    head = b""
    tail = b""
    try:
        with open(tmp, "wb") as out:
            while chunk := stream.read(1 << 20):
                size += len(chunk)
                if size > max_bytes:
                    raise UploadTooLarge(f"upload exceeds {max_bytes} bytes")
                if len(head) < 4096:
                    head += chunk[: 4096 - len(head)]
                tail = (tail + chunk)[-8:]
                h.update(chunk)
                out.write(chunk)
        if size == 0:
            raise UnsupportedContent("empty upload")
        kind = sniff(head, tail)
        final = directory / f"{upload_id}{EXTENSION[kind]}"
        tmp.rename(final)
        return upload_id, final, size, h.hexdigest(), kind
    finally:
        tmp.unlink(missing_ok=True)
