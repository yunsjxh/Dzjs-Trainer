import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = readFileSync(path.join(root, '../DzjsTrainerUI/MainWindow.cpp'), 'utf8');
const take = (start, end) => {
  const a = source.indexOf(start);
  const b = source.indexOf(end, a + start.length);
  if (a < 0 || b < 0) throw new Error(`Source boundary missing: ${start}`);
  return source.slice(a, b);
};
const template = readFileSync(path.join(root, 'tools/ui-preview.cpp.in'), 'utf8');
const helpers = take('const wchar_t* kNavLabels[]', 'bool Hit(const RECT&');
const methods = take('void MainWindow::PaintNavigation', 'void MainWindow::PaintAntivirus');
mkdirSync(path.join(root, '.verification'), { recursive: true });
mkdirSync(path.join(root, 'assets'), { recursive: true });
writeFileSync(path.join(root, '.verification/ui-preview.cpp'), template.replace('/* SOURCE_HELPERS */', helpers).replace('/* SOURCE_METHODS */', methods));
writeFileSync(path.join(root, '.verification/ui-source.json'), JSON.stringify({
  source: 'DzjsTrainerUI/MainWindow.cpp',
  sha256: createHash('sha256').update(source).digest('hex'),
  methods: ['PaintNavigation', 'PaintHeader', 'PaintOverview', 'PaintProtection'],
  method: 'Unmodified current GDI+ paint methods rendered with a disconnected preview state. No application workers or drivers run.',
  versionNote: 'The native sidebar retains v1.7.6. Website release version v1.0.0 comes from README.md.'
}, null, 2));
console.log('UI_SOURCE_EXTRACTED: 4 native paint methods; original source unchanged');
