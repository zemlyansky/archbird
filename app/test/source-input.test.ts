import assert from "node:assert/strict";
import { File } from "node:buffer";
import test from "node:test";
import { strToU8, zipSync } from "fflate";
import {
  SOURCE_LIMITS,
  extractZipBytes,
  normalizeRepositoryPath,
  readDirectoryFiles,
} from "../src/sources/input";

function directoryFile(path: string, source: string): globalThis.File {
  const file = new File([source], path.split("/").at(-1) || "source") as unknown as globalThis.File;
  Object.defineProperty(file, "webkitRelativePath", { value: path });
  return file;
}

test("directory input strips only the user-selected root and sorts paths", async () => {
  const files = [
    directoryFile("fixture/src/z.c", "int z;\n"),
    directoryFile("fixture/README.md", "fixture\n"),
    directoryFile("fixture/src/a.c", "int a;\n"),
  ];
  const progress: string[] = [];
  const rows = await readDirectoryFiles(files, (row) => progress.push(`${row.phase}:${row.completed}`));
  assert.deepEqual(rows.map((row) => row.path), ["README.md", "src/a.c", "src/z.c"]);
  assert.equal(new TextDecoder().decode(rows[1].data), "int a;\n");
  assert.deepEqual(progress, ["inventory:0", "read:3"]);
});

test("bounded ZIP input strips a shared archive root and retains exact bytes", () => {
  const archive = zipSync({
    "fixture/README.md": strToU8("fixture\n"),
    "fixture/src/a.c": strToU8("int a;\n"),
  });
  const rows = extractZipBytes(archive);
  assert.deepEqual(rows.map((row) => row.path), ["README.md", "src/a.c"]);
  assert.equal(new TextDecoder().decode(rows[1].data), "int a;\n");
});

test("repository and ZIP paths reject traversal and absolute identities", () => {
  for (const path of ["../escape.c", "root/../../escape.c", "/absolute.c", "C:/drive.c"]) {
    assert.throws(() => normalizeRepositoryPath(path), /unsafe repository path/);
  }
  const archive = zipSync({ "../escape.c": strToU8("bad\n") });
  assert.throws(() => extractZipBytes(archive), /unsafe repository path/);
});

test("ZIP central-directory size claims are bounded before inflation", () => {
  const archive = zipSync({ "fixture/a.c": strToU8("int a;\n") });
  const view = new DataView(archive.buffer, archive.byteOffset, archive.byteLength);
  let central = -1;
  for (let offset = 0; offset + 46 <= archive.length; offset += 1) {
    if (view.getUint32(offset, true) === 0x02014b50) {
      central = offset;
      break;
    }
  }
  assert.notEqual(central, -1);
  view.setUint32(central + 24, SOURCE_LIMITS.fileBytes + 1, true);
  assert.throws(() => extractZipBytes(archive), /browser file limit/);
});
