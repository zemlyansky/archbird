"use strict";

const { Buffer } = require("buffer");
const ts = require("typescript");

const CONFIGURATION = Object.freeze({
  allowJs: true,
  checkJs: false,
  diagnostics: "syntactic-only",
  fact_domains: ["semantic-definitions", "reference-targets"],
  jsx: "preserve",
  module: "commonjs",
  moduleResolution: "node10",
  noEmit: true,
  noLib: true,
  schema_version: 1,
  target: "latest",
});

function utf8Compare(left, right) {
  return Buffer.compare(Buffer.from(left), Buffer.from(right));
}

function canonical(value) {
  if (Array.isArray(value)) return value.map(canonical);
  if (value && typeof value === "object" && !Buffer.isBuffer(value)) {
    return Object.fromEntries(
      Object.keys(value)
        .sort(utf8Compare)
        .map((key) => [key, canonical(value[key])]),
    );
  }
  return value;
}

function canonicalBytes(value) {
  return Buffer.from(JSON.stringify(canonical(value)));
}

function framed(parts, value) {
  const bytes = Buffer.from(value);
  const length = Buffer.alloc(8);
  length.writeBigUInt64BE(BigInt(bytes.length));
  parts.push(length, bytes);
}

function factId(hashBytes, project, path, domain, kind, start, end, key) {
  const parts = [];
  for (const value of [project, path, domain, kind]) framed(parts, value);
  const numbers = Buffer.alloc(16);
  numbers.writeBigUInt64BE(BigInt(start), 0);
  numbers.writeBigUInt64BE(BigInt(end), 8);
  parts.push(numbers);
  framed(parts, key);
  return `f:${hashBytes(Buffer.concat(parts))}`;
}

function byteOffsets(text) {
  const result = new Uint32Array(text.length + 1);
  let byte = 0;
  let index = 0;
  while (index < text.length) {
    result[index] = byte;
    const codePoint = text.codePointAt(index);
    const width = codePoint > 0xffff ? 2 : 1;
    if (width === 2) result[index + 1] = byte;
    byte += Buffer.byteLength(String.fromCodePoint(codePoint));
    index += width;
    result[index] = byte;
  }
  return result;
}

function canonicalPath(value) {
  return value.replaceAll("\\", "/").replace(/^\.\//, "");
}

function definitionDeclaration(node) {
  if (!node) return false;
  return (
    ts.isFunctionDeclaration(node) ||
    ts.isFunctionExpression(node) ||
    ts.isClassDeclaration(node) ||
    ts.isClassExpression(node) ||
    ts.isInterfaceDeclaration(node) ||
    ts.isTypeAliasDeclaration(node) ||
    ts.isEnumDeclaration(node) ||
    ts.isEnumMember(node) ||
    ts.isMethodDeclaration(node) ||
    ts.isMethodSignature(node) ||
    ts.isGetAccessorDeclaration(node) ||
    ts.isSetAccessorDeclaration(node) ||
    ts.isPropertyDeclaration(node) ||
    ts.isPropertySignature(node) ||
    ts.isPropertyAssignment(node) ||
    ts.isShorthandPropertyAssignment(node) ||
    ts.isParameter(node) ||
    ts.isVariableDeclaration(node) ||
    ts.isImportClause(node) ||
    ts.isImportSpecifier(node) ||
    ts.isNamespaceImport(node) ||
    ts.isBindingElement(node) ||
    ts.isTypeParameterDeclaration(node) ||
    ts.isModuleDeclaration(node)
  );
}

function declarationName(node) {
  return Boolean(
    node &&
      node.parent &&
      definitionDeclaration(node.parent) &&
      node.parent.name === node,
  );
}

function definitionKind(parent) {
  if (!parent) return "binding";
  if (ts.isFunctionDeclaration(parent) || ts.isFunctionExpression(parent))
    return "function";
  if (ts.isClassDeclaration(parent) || ts.isClassExpression(parent))
    return "class";
  if (ts.isInterfaceDeclaration(parent)) return "interface";
  if (ts.isTypeAliasDeclaration(parent)) return "type";
  if (ts.isEnumDeclaration(parent)) return "enum";
  if (ts.isMethodDeclaration(parent) || ts.isMethodSignature(parent))
    return "method";
  if (ts.isParameter(parent)) return "parameter";
  if (
    ts.isImportClause(parent) ||
    ts.isImportSpecifier(parent) ||
    ts.isNamespaceImport(parent)
  )
    return "import";
  if (ts.isPropertyDeclaration(parent) || ts.isPropertySignature(parent))
    return "property";
  if (ts.isTypeParameterDeclaration(parent)) return "type-parameter";
  return "binding";
}

function definitionScope(parent) {
  if (!parent) return "binding";
  if (
    ts.isMethodDeclaration(parent) ||
    ts.isMethodSignature(parent) ||
    ts.isPropertyDeclaration(parent) ||
    ts.isPropertySignature(parent)
  ) {
    return "member";
  }
  if (
    ts.isClassDeclaration(parent) ||
    ts.isClassExpression(parent) ||
    ts.isInterfaceDeclaration(parent)
  ) {
    return "type";
  }
  if (ts.isFunctionDeclaration(parent) || ts.isFunctionExpression(parent)) {
    return "function";
  }
  return "binding";
}

function namedContainer(node) {
  return (
    ts.isClassDeclaration(node) ||
    ts.isClassExpression(node) ||
    ts.isInterfaceDeclaration(node) ||
    ts.isModuleDeclaration(node) ||
    ts.isFunctionDeclaration(node) ||
    ts.isFunctionExpression(node) ||
    ts.isMethodDeclaration(node) ||
    ts.isMethodSignature(node)
  );
}

function qualifiedDefinitionName(parent, leaf, sourceFile) {
  const parts = [leaf];
  let current = parent?.parent;
  while (current && current !== sourceFile) {
    if (namedContainer(current) && current.name) {
      const name = current.name.getText(sourceFile);
      if (name) parts.push(name);
    }
    current = current.parent;
  }
  return parts.reverse().join(".");
}

function definitionSignature(parent, qualified, sourceFile) {
  const leaf = qualified.split(".").at(-1);
  if (!parent) return leaf;
  if (
    ts.isFunctionDeclaration(parent) ||
    ts.isFunctionExpression(parent) ||
    ts.isMethodDeclaration(parent) ||
    ts.isMethodSignature(parent)
  ) {
    const parameters = parent.parameters
      .map((parameter) => parameter.name.getText(sourceFile))
      .join(", ");
    return `${leaf}(${parameters})`;
  }
  if (ts.isClassDeclaration(parent) || ts.isClassExpression(parent)) {
    return `class ${leaf}`;
  }
  if (ts.isInterfaceDeclaration(parent)) return `interface ${leaf}`;
  if (ts.isTypeAliasDeclaration(parent)) return `type ${leaf}`;
  if (ts.isEnumDeclaration(parent)) return `enum ${leaf}`;
  return leaf;
}

function definitionRecord(declaration, sourceMap, offsetsByPath) {
  if (!declaration) return null;
  const sourceFile = declaration.getSourceFile();
  const path = canonicalPath(sourceFile.fileName);
  if (!sourceMap.has(path)) return null;
  let parent = definitionDeclaration(declaration) ? declaration : null;
  let nameNode = parent?.name;
  if (!nameNode && ts.isIdentifier(declaration) && declarationName(declaration)) {
    parent = declaration.parent;
    nameNode = declaration;
  }
  if (!nameNode) return null;
  const leaf = nameNode.getText(sourceFile);
  if (!leaf) return null;
  const offsets = offsetsByPath.get(path);
  const start = offsets[nameNode.getStart(sourceFile)];
  const end = offsets[nameNode.getEnd()];
  return {
    end,
    kind: definitionKind(parent),
    leaf,
    path,
    qualified: qualifiedDefinitionName(parent, leaf, sourceFile),
    scope: definitionScope(parent),
    signature: definitionSignature(
      parent,
      qualifiedDefinitionName(parent, leaf, sourceFile),
      sourceFile,
    ),
    start,
    syntaxKind: ts.SyntaxKind[parent.kind],
  };
}

function referenceKind(node) {
  const parent = node.parent;
  const expression =
    parent && ts.isPropertyAccessExpression(parent) && parent.name === node
      ? parent
      : node;
  const use = expression.parent;
  if (use && ts.isCallExpression(use) && use.expression === expression)
    return "call";
  if (use && ts.isNewExpression(use) && use.expression === expression)
    return "construct";
  if (use && ts.isTaggedTemplateExpression(use) && use.tag === expression)
    return "call";
  if (parent && ts.isDecorator(parent)) return "decorator";
  if (
    (parent && ts.isTypeReferenceNode(parent)) ||
    (parent && ts.isExpressionWithTypeArguments(parent)) ||
    (parent && ts.isTypeQueryNode(parent))
  ) {
    return "type";
  }
  return "reference";
}

function enclosingDefinition(node, sourceFile) {
  let current = node.parent;
  while (current && current !== sourceFile) {
    if (definitionDeclaration(current) && current.name) {
      const leaf = current.name.getText(sourceFile);
      if (leaf) return qualifiedDefinitionName(current, leaf, sourceFile);
    }
    current = current.parent;
  }
  return "";
}

function makeFact(
  hashBytes,
  project,
  path,
  domain,
  kind,
  claim,
  span,
  key,
  name,
  attributes = {},
) {
  const row = {
    claim,
    correlation: "span",
    domain,
    id: factId(
      hashBytes,
      project,
      path,
      domain,
      kind,
      span.start,
      span.end,
      key,
    ),
    key,
    kind,
    path,
    project,
    span,
  };
  if (name !== undefined) row.name = name;
  if (Object.keys(attributes).length) row.attributes = attributes;
  return row;
}

function compilerHost(sourceMap, options) {
  return {
    directoryExists: () => true,
    fileExists: (fileName) => sourceMap.has(canonicalPath(fileName)),
    getCanonicalFileName: (fileName) => canonicalPath(fileName),
    getCurrentDirectory: () => "",
    getDefaultLibFileName: () => "",
    getDirectories: () => [],
    getNewLine: () => "\n",
    getSourceFile: (fileName, languageVersion) => {
      const source = sourceMap.get(canonicalPath(fileName));
      if (!source) return undefined;
      const kind = source.path.endsWith(".tsx")
        ? ts.ScriptKind.TSX
        : source.path.endsWith(".jsx")
          ? ts.ScriptKind.JSX
          : source.language === "typescript"
            ? ts.ScriptKind.TS
            : ts.ScriptKind.JS;
      return ts.createSourceFile(
        source.path,
        source.text,
        languageVersion,
        true,
        kind,
      );
    },
    readFile: (fileName) => sourceMap.get(canonicalPath(fileName))?.text,
    realpath: canonicalPath,
    useCaseSensitiveFileNames: () => true,
    writeFile: () => {},
  };
}

function mappedTarget(symbol, sourceMap, offsetsByPath, hashBytes) {
  if (!symbol) return { attributes: {}, state: "unresolved", targets: [] };
  const declarations = symbol.declarations || [];
  const mapped = [];
  const external = [];
  let unsupportedMapped = false;
  for (const declaration of declarations) {
    const sourceFile = declaration.getSourceFile();
    const path = canonicalPath(sourceFile.fileName);
    if (!sourceMap.has(path)) {
      external.push({
        file: path,
        kind: ts.SyntaxKind[declaration.kind],
        name: symbol.getName(),
      });
      continue;
    }
    const record = definitionRecord(declaration, sourceMap, offsetsByPath);
    if (record) {
      mapped.push(record);
    } else {
      unsupportedMapped = true;
    }
  }
  mapped.sort((left, right) =>
    utf8Compare(
      `${left.path}\0${left.qualified}\0${left.start}\0${left.end}`,
      `${right.path}\0${right.qualified}\0${right.start}\0${right.end}`,
    ),
  );
  external.sort((left, right) =>
    utf8Compare(JSON.stringify(left), JSON.stringify(right)),
  );
  const paths = [...new Set(mapped.map((item) => item.path))];
  if (unsupportedMapped) {
    return { attributes: {}, state: "unknown", targets: [] };
  }
  if (paths.length === 1 && external.length === 0) {
    const names = [...new Set(mapped.map((item) => item.qualified))];
    const targetSymbol = names.length === 1 ? names[0] : symbol.getName();
    return {
      attributes: {
        target_path: paths[0],
        target_symbol: targetSymbol,
      },
      state: "unique",
      targets: [`symbol:${hashBytes(canonicalBytes(mapped))}`],
    };
  }
  const targets = paths.map((path) => {
    const rows = mapped.filter((item) => item.path === path);
    return `symbol:${hashBytes(canonicalBytes(rows))}`;
  });
  if (external.length) {
    targets.push(`external:${hashBytes(canonicalBytes(external))}`);
  }
  targets.sort(utf8Compare);
  if (targets.length >= 2) {
    return { attributes: {}, state: "ambiguous", targets };
  }
  if (targets.length === 1 && external.length) {
    return { attributes: {}, state: "external", targets };
  }
  return { attributes: {}, state: "unresolved", targets: [] };
}

function* typescriptProviderBundles({
  project,
  sources,
  sourceManifestSha256,
  hashBytes,
  implementationSha256,
  runtime,
}) {
  if (typeof hashBytes !== "function") {
    throw new TypeError(
      "TypeScript provider requires a synchronous SHA-256 function",
    );
  }
  if (!/^[0-9a-f]{64}$/.test(implementationSha256 || "")) {
    throw new TypeError(
      "TypeScript provider requires its implementation SHA-256",
    );
  }
  if (typeof runtime !== "string" || !runtime) {
    throw new TypeError("TypeScript provider requires a runtime identity");
  }
  const supported = sources
    .filter((source) => ["javascript", "typescript"].includes(source.language))
    .map((source) => ({
      language: source.language,
      path: canonicalPath(source.path),
      text: Buffer.from(source.data).toString("utf8"),
    }))
    .sort((left, right) => utf8Compare(left.path, right.path));
  const sourceMap = new Map(supported.map((source) => [source.path, source]));
  const offsetsByPath = new Map(
    supported.map((source) => [source.path, byteOffsets(source.text)]),
  );
  const options = {
    allowJs: true,
    checkJs: false,
    module: ts.ModuleKind.CommonJS,
    moduleResolution: ts.ModuleResolutionKind.Node10,
    noEmit: true,
    noLib: true,
    skipLibCheck: true,
    target: ts.ScriptTarget.Latest,
    jsx: ts.JsxEmit.Preserve,
  };
  const program = ts.createProgram({
    host: compilerHost(sourceMap, options),
    options,
    rootNames: supported.map((source) => source.path),
  });
  const checker = program.getTypeChecker();
  const diagnosticsByPath = new Map();
  // This in-memory provider deliberately has no dependency tree or default
  // library.  Only parser diagnostics are repository evidence in that
  // environment; semantic/module diagnostics would mostly describe the
  // provider boundary, not defects in the supplied source.
  const diagnostics = program
    .getSyntacticDiagnostics()
    .filter(
      (diagnostic) =>
        diagnostic.file &&
        sourceMap.has(canonicalPath(diagnostic.file.fileName)),
    )
    .map((diagnostic) => {
      const path = canonicalPath(diagnostic.file.fileName);
      const row = {
        code: `typescript-${diagnostic.code}`,
        message: ts.flattenDiagnosticMessageText(diagnostic.messageText, "\n"),
        path,
        project,
        severity:
          diagnostic.category === ts.DiagnosticCategory.Error
            ? "error"
            : diagnostic.category === ts.DiagnosticCategory.Warning
              ? "warning"
              : "note",
      };
      if (typeof diagnostic.start === "number") {
        const offsets = offsetsByPath.get(path);
        const endCharacter = diagnostic.start + (diagnostic.length || 0);
        row.span = {
          end: offsets[Math.min(endCharacter, offsets.length - 1)],
          start: offsets[Math.min(diagnostic.start, offsets.length - 1)],
        };
      }
      return row;
    })
    .sort((left, right) => {
      for (const key of ["code", "project", "path"]) {
        const compared = utf8Compare(left[key] || "", right[key] || "");
        if (compared) return compared;
      }
      for (const value of [
        (left.span?.start || 0) - (right.span?.start || 0),
        (left.span?.end || 0) - (right.span?.end || 0),
      ]) {
        if (value) return value;
      }
      for (const key of ["severity", "message"]) {
        const compared = utf8Compare(left[key] || "", right[key] || "");
        if (compared) return compared;
      }
      return 0;
    });
  for (const diagnostic of diagnostics) {
    if (!diagnosticsByPath.has(diagnostic.path)) {
      diagnosticsByPath.set(diagnostic.path, []);
    }
    diagnosticsByPath.get(diagnostic.path).push(diagnostic);
  }
  for (const sourceFile of program.getSourceFiles()) {
    const path = canonicalPath(sourceFile.fileName);
    if (!sourceMap.has(path)) continue;
    const offsets = offsetsByPath.get(path);
    const facts = [];
    const resolutions = [];
    function span(node) {
      return {
        end: offsets[node.getEnd()],
        start: offsets[node.getStart(sourceFile)],
      };
    }
    function visit(node) {
      if (ts.isIdentifier(node) && node.text && declarationName(node)) {
        const range = span(node);
        const definition = definitionRecord(
          node.parent,
          sourceMap,
          offsetsByPath,
        );
        if (definition) {
          facts.push(
            makeFact(
              hashBytes,
              project,
              path,
              "semantic-definitions",
              definition.kind,
              "syntax-structure",
              range,
              definition.qualified,
              definition.qualified,
              {
                line:
                  sourceFile.getLineAndCharacterOfPosition(
                    node.getStart(sourceFile),
                  ).line + 1,
                scope: definition.scope,
                signature: definition.signature,
                syntax_kind: definition.syntaxKind,
              },
            ),
          );
        }
      } else if (ts.isIdentifier(node) && node.text) {
        const range = span(node);
        let symbol = checker.getSymbolAtLocation(node);
        if (symbol && symbol.flags & ts.SymbolFlags.Alias) {
          const aliased = checker.getAliasedSymbol(symbol);
          if (aliased) symbol = aliased;
        }
        const target = mappedTarget(
          symbol,
          sourceMap,
          offsetsByPath,
          hashBytes,
        );
        const kind = referenceKind(node);
          const attributes = {
            declaration_count: symbol?.declarations?.length || 0,
            enclosing: enclosingDefinition(node, sourceFile),
          reference_kind: kind,
          symbol_flags: symbol ? symbol.flags : 0,
          ...target.attributes,
        };
        const fact = makeFact(
          hashBytes,
          project,
          path,
          "reference-targets",
          kind,
          "semantic-target",
          range,
          `${node.text}:${range.start}:${range.end}`,
          node.text,
          attributes,
        );
        facts.push(fact);
        resolutions.push({
          fact_id: fact.id,
          state: target.state,
          targets: target.targets,
        });
      }
      ts.forEachChild(node, visit);
    }
    visit(sourceFile);
    facts.sort((left, right) => utf8Compare(left.id, right.id));
    resolutions.sort((left, right) => utf8Compare(left.fact_id, right.fact_id));
    yield canonicalBytes({
      artifact: "archbird-provider-facts",
      capabilities: [
        {
          boundary:
            "TypeScript compiler targets from one supplied JS/TS file into the supplied project; no default library, installed dependency tree, or dynamic dispatch",
          claims: ["semantic-target"],
          coverage: "bounded",
          domain: "reference-targets",
        },
        {
          boundary:
            "TypeScript compiler syntax declarations in one supplied JS/TS file",
          claims: ["syntax-structure"],
          coverage: "complete",
          domain: "semantic-definitions",
        },
      ],
      diagnostics: diagnosticsByPath.get(path) || [],
      facts,
      inputs: [{ project, source_manifest_sha256: sourceManifestSha256 }],
      producer: {
        configuration_sha256: hashBytes(canonicalBytes(CONFIGURATION)),
        implementation_sha256: implementationSha256,
        name: "archbird-typescript",
        runtime,
        version: "1",
      },
      provenance: "derived",
      resolutions,
      schema_version: 1,
      subject: { path, project, scope: "file" },
    });
  }
}

module.exports = { typescriptProviderBundles };
