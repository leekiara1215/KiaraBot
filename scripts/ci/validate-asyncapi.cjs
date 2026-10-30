const fs = require('node:fs');
const { Parser } = require('@asyncapi/parser');

async function main() {
  const file = process.argv[2];
  if (!file) throw new Error('usage: node validate-asyncapi.cjs FILE');
  const { document, diagnostics } = await new Parser().parse(fs.readFileSync(file, 'utf8'), {
    source: file,
  });
  for (const diagnostic of diagnostics) {
    console.error(`${diagnostic.severity}: ${diagnostic.message}`);
  }
  if (!document || diagnostics.some((diagnostic) => diagnostic.severity === 0)) {
    process.exitCode = 1;
    return;
  }
  console.log(`AsyncAPI parser validation passed: ${file}`);
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
