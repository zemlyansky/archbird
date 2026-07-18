import { unzipSync } from "fflate";

export const SOURCE_LIMITS = Object.freeze({
  archiveBytes: 128 * 1024 * 1024,
  files: 20_000,
  fileBytes: 32 * 1024 * 1024,
  totalBytes: 512 * 1024 * 1024,
});

export interface RepositoryInputFile {
  path: string;
  data: Uint8Array;
}

export interface SourceProgress {
  completed: number;
  phase: "inventory" | "read" | "unpack";
  total: number;
}

interface ZipEntry {
  compressed: number;
  directory: boolean;
  path: string;
  uncompressed: number;
}

const textEncoder = new TextEncoder();

function utf8Compare(left: string, right: string): number {
  const a = textEncoder.encode(left);
  const b = textEncoder.encode(right);
  const common = Math.min(a.length, b.length);
  for (let index = 0; index < common; index += 1) {
    if (a[index] !== b[index]) return a[index] - b[index];
  }
  return a.length - b.length;
}

export function normalizeRepositoryPath(value: string): string {
  const normalized = value.replaceAll("\\", "/");
  if (
    !normalized || normalized.startsWith("/") || /^[A-Za-z]:\//.test(normalized)
    || normalized.includes("\0")
  ) {
    throw new Error(`unsafe repository path: ${JSON.stringify(value)}`);
  }
  const parts = normalized.split("/").filter((part) => part !== "");
  if (!parts.length || parts.some((part) => part === "." || part === "..")) {
    throw new Error(`unsafe repository path: ${JSON.stringify(value)}`);
  }
  if (parts.some((part) => /[\u0000-\u001f\u007f]/.test(part))) {
    throw new Error(`repository path contains control characters: ${JSON.stringify(value)}`);
  }
  return parts.join("/");
}

function stripSharedRoot(paths: string[]): string[] {
  if (!paths.length) return [];
  const split = paths.map((path) => path.split("/"));
  const root = split[0][0];
  if (!root || split.some((parts) => parts.length < 2 || parts[0] !== root)) {
    return paths;
  }
  return split.map((parts) => parts.slice(1).join("/"));
}

function validateRows(rows: Array<{ path: string; bytes: number }>): void {
  if (!rows.length) throw new Error("repository input contains no files");
  if (rows.length > SOURCE_LIMITS.files) {
    throw new Error(`repository input exceeds ${SOURCE_LIMITS.files} files`);
  }
  let total = 0;
  const seen = new Set<string>();
  for (const row of rows) {
    if (row.bytes > SOURCE_LIMITS.fileBytes) {
      throw new Error(`${row.path} exceeds the ${SOURCE_LIMITS.fileBytes}-byte browser file limit`);
    }
    total += row.bytes;
    if (total > SOURCE_LIMITS.totalBytes) {
      throw new Error(`repository input exceeds ${SOURCE_LIMITS.totalBytes} uncompressed bytes`);
    }
    if (seen.has(row.path)) throw new Error(`duplicate repository path: ${row.path}`);
    seen.add(row.path);
  }
}

export async function readDirectoryFiles(
  files: readonly File[],
  progress: (progress: SourceProgress) => void = () => undefined,
): Promise<RepositoryInputFile[]> {
  if (!files.length) throw new Error("selected directory contains no files");
  const rawPaths = files.map((file) => normalizeRepositoryPath(file.webkitRelativePath || file.name));
  const normalized = files.every((file) => Boolean(file.webkitRelativePath))
    ? stripSharedRoot(rawPaths)
    : rawPaths;
  const rows = files.map((file, index) => ({
    bytes: file.size,
    file,
    path: normalizeRepositoryPath(normalized[index]),
  })).sort((left, right) => utf8Compare(left.path, right.path));
  validateRows(rows);
  progress({ completed: 0, phase: "inventory", total: rows.length });
  const result: RepositoryInputFile[] = [];
  for (const [index, row] of rows.entries()) {
    result.push({ path: row.path, data: new Uint8Array(await row.file.arrayBuffer()) });
    if ((index + 1) % 32 === 0 || index + 1 === rows.length) {
      progress({ completed: index + 1, phase: "read", total: rows.length });
    }
  }
  return result;
}

function findEndOfCentralDirectory(bytes: Uint8Array): number {
  const minimum = Math.max(0, bytes.length - 65_557);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  for (let offset = bytes.length - 22; offset >= minimum; offset -= 1) {
    if (view.getUint32(offset, true) === 0x06054b50) return offset;
  }
  throw new Error("ZIP end-of-central-directory record is missing");
}

function decodeZipName(bytes: Uint8Array, utf8: boolean): string {
  if (!utf8 && bytes.some((value) => value > 0x7f)) {
    throw new Error("ZIP entry names must be UTF-8 or ASCII");
  }
  try {
    return new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  } catch (error) {
    throw new Error(`ZIP entry name is not valid UTF-8: ${(error as Error).message}`);
  }
}

function zipEntries(bytes: Uint8Array): ZipEntry[] {
  if (bytes.byteLength > SOURCE_LIMITS.archiveBytes) {
    throw new Error(`ZIP exceeds the ${SOURCE_LIMITS.archiveBytes}-byte archive limit`);
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const end = findEndOfCentralDirectory(bytes);
  const disk = view.getUint16(end + 4, true);
  const centralDisk = view.getUint16(end + 6, true);
  const diskEntries = view.getUint16(end + 8, true);
  const entries = view.getUint16(end + 10, true);
  const centralBytes = view.getUint32(end + 12, true);
  const centralOffset = view.getUint32(end + 16, true);
  if (
    disk !== 0 || centralDisk !== 0 || diskEntries !== entries
    || entries === 0xffff || centralBytes === 0xffffffff || centralOffset === 0xffffffff
  ) {
    throw new Error("multi-disk and ZIP64 archives are not supported in browser mode");
  }
  if (entries > SOURCE_LIMITS.files || centralOffset + centralBytes > end) {
    throw new Error("ZIP central directory exceeds browser limits or archive bounds");
  }
  const result: ZipEntry[] = [];
  let offset = centralOffset;
  for (let index = 0; index < entries; index += 1) {
    if (offset + 46 > end || view.getUint32(offset, true) !== 0x02014b50) {
      throw new Error(`ZIP central directory entry ${index} is invalid`);
    }
    const flags = view.getUint16(offset + 8, true);
    const method = view.getUint16(offset + 10, true);
    const compressed = view.getUint32(offset + 20, true);
    const uncompressed = view.getUint32(offset + 24, true);
    const nameBytes = view.getUint16(offset + 28, true);
    const extraBytes = view.getUint16(offset + 30, true);
    const commentBytes = view.getUint16(offset + 32, true);
    const externalAttributes = view.getUint32(offset + 38, true);
    const next = offset + 46 + nameBytes + extraBytes + commentBytes;
    if (next > end) throw new Error(`ZIP central directory entry ${index} is truncated`);
    if (flags & 1) throw new Error("encrypted ZIP entries are not supported");
    if (method !== 0 && method !== 8) {
      throw new Error(`ZIP compression method ${method} is not supported`);
    }
    const mode = externalAttributes >>> 16;
    if ((mode & 0xf000) === 0xa000) throw new Error("ZIP symbolic links are not accepted");
    const rawName = decodeZipName(
      bytes.subarray(offset + 46, offset + 46 + nameBytes),
      Boolean(flags & 0x0800),
    );
    const directory = rawName.endsWith("/");
    const path = normalizeRepositoryPath(directory ? rawName.slice(0, -1) : rawName);
    result.push({ compressed, directory, path, uncompressed });
    offset = next;
  }
  if (offset !== centralOffset + centralBytes) {
    throw new Error("ZIP central directory size does not match its entries");
  }
  return result;
}

export function extractZipBytes(bytes: Uint8Array): RepositoryInputFile[] {
  const entries = zipEntries(bytes);
  const files = entries.filter((entry) => !entry.directory);
  const stripped = stripSharedRoot(files.map((entry) => entry.path));
  const rows = files.map((entry, index) => ({
    bytes: entry.uncompressed,
    entry,
    path: normalizeRepositoryPath(stripped[index]),
  }));
  validateRows(rows);
  for (const row of rows) {
    if (row.entry.uncompressed && row.entry.compressed === 0) {
      throw new Error(`ZIP entry has an invalid zero compressed size: ${row.path}`);
    }
    if (row.entry.compressed && row.entry.uncompressed / row.entry.compressed > 1_000) {
      throw new Error(`ZIP entry exceeds the 1000:1 expansion limit: ${row.path}`);
    }
  }
  let unpacked: Record<string, Uint8Array>;
  try {
    unpacked = unzipSync(bytes);
  } catch (error) {
    throw new Error(`cannot unpack ZIP: ${(error as Error).message}`);
  }
  const byPath = new Map<string, Uint8Array>();
  for (const [raw, data] of Object.entries(unpacked)) {
    if (raw.endsWith("/")) continue;
    byPath.set(normalizeRepositoryPath(raw), data);
  }
  const result = rows.map((row) => {
    const data = byPath.get(row.entry.path);
    if (!data || data.byteLength !== row.entry.uncompressed) {
      throw new Error(`ZIP output does not match central-directory evidence: ${row.entry.path}`);
    }
    return { path: row.path, data };
  }).sort((left, right) => utf8Compare(left.path, right.path));
  validateRows(result.map((row) => ({ path: row.path, bytes: row.data.byteLength })));
  return result;
}

export async function readZipFile(
  file: File,
  progress: (progress: SourceProgress) => void = () => undefined,
): Promise<RepositoryInputFile[]> {
  if (file.size > SOURCE_LIMITS.archiveBytes) {
    throw new Error(`ZIP exceeds the ${SOURCE_LIMITS.archiveBytes}-byte archive limit`);
  }
  progress({ completed: 0, phase: "read", total: 1 });
  const bytes = new Uint8Array(await file.arrayBuffer());
  progress({ completed: 1, phase: "read", total: 1 });
  const result = extractZipBytes(bytes);
  progress({ completed: result.length, phase: "unpack", total: result.length });
  return result;
}
