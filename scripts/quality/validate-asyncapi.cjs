'use strict';

const fs = require('node:fs');
const { Parser, DiagnosticSeverity } = require(process.argv[2]);

async function main() {
  const source = fs.readFileSync(process.argv[3], 'utf8');
  const { document, diagnostics } = await new Parser().parse(source, { source: process.argv[3] });
  for (const diagnostic of diagnostics) {
    console.error(`${DiagnosticSeverity[diagnostic.severity]}: ${diagnostic.message} (${diagnostic.path.join('.')})`);
  }
  if (!document || diagnostics.some((diagnostic) => diagnostic.severity === DiagnosticSeverity.Error)) {
    process.exitCode = 1;
    return;
  }
  console.log('AsyncAPI parser validation passed');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
