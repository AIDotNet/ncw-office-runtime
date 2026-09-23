/**
 * 诊断脚本(只给 CI 用):依次打开几种文档,若 helper 卡住,就用 Windows 调试器 cdb 抓栈。
 *
 * 需求:Windows 上 LibreOfficeKit 加载 Calc / Impress / Draw 文档时 documentLoad 永不返回
 * (Writer 正常;soffice --headless 正常;环境变量、Batch=true 都无效)。第一轮抓栈确认了 Calc
 * 是公式栏子窗口跨线程 SetWindowPos 死锁(已在 helper 里预置配置隐藏公式栏);这里继续看
 * Impress / Draw 卡在哪个子窗口,以及主循环线程在等什么。
 *
 * 用法:node test/diag-hang.mjs xlsx pptx pdf
 * 输出:每种文档的结果;卡住时附线程 0(helper 线程)与线程 1(LibreOffice 主循环)的栈。
 * 定位并修复后连同 build.yml 里的诊断步骤一起删掉。
 */
import { spawnSync } from 'node:child_process'
import { copyFileSync, existsSync, mkdtempSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { HelperClient } from './protocol-client.mjs'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const kinds = process.argv.length > 2 ? process.argv.slice(2) : ['xlsx']
const helper = join(root, 'native', `${process.platform}-${process.arch}`, process.platform === 'win32' ? 'ncw-office-helper.exe' : 'ncw-office-helper')
const cdb = [
  'C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x64\\cdb.exe',
  'C:\\Program Files\\Windows Kits\\10\\Debuggers\\x64\\cdb.exe'
].find((candidate) => existsSync(candidate))

/** 把 C++ 模板实参折叠成 <…>:栈帧里一行 COM 模板名能有两千字符,annotation 只有 4096 */
function shorten(line) {
  let out = line
  for (let i = 0; i < 20; i++) {
    const next = out.replace(/<[^<>]*>/g, '‹›')
    if (next === out) break
    out = next
  }
  return out.replace(/‹›/g, '<…>').replace(/^([0-9a-f]{2}) [0-9a-f`]+ [0-9a-f`]+\s+/i, '$1 ')
}

for (const kind of kinds) {
  const work = mkdtempSync(join(tmpdir(), 'ncw-diag-'))
  const file = join(work, `diag.${kind}`)
  copyFileSync(join(root, 'test-fixtures', `blank.${kind}`), file)
  const client = await HelperClient.start({ helper, loPath: process.env.NCW_LO_PATH, workDir: join(work, 'helper') })
  const opened = client.request('document.open', { path: file, format: kind }).then(() => 'opened', (error) => `failed: ${error.message.split('\n')[0]}`)
  const outcome = await Promise.race([opened, new Promise((r) => { setTimeout(() => { r('hung') }, 45_000) })])
  console.log(`== open ${kind}: ${outcome}`)
  if (outcome === 'hung') {
    if (cdb === undefined) {
      console.log('cdb.exe not found on this runner')
    } else {
      const result = spawnSync(cdb, ['-p', String(client.child.pid), '-c', `.symfix ${join(work, 'sym')};.reload;~0kn 30;~1kn 30;qd`], {
        encoding: 'utf8',
        timeout: 240_000,
        maxBuffer: 64 * 1024 * 1024
      })
      const lines = `${result.stdout ?? ''}${result.stderr ?? ''}`.split(/\r?\n/)
      console.log(lines.filter((line) => / Id: /.test(line) || /^\s*[0-9a-f]{2} [0-9a-f`]{8,}/i.test(line)).map(shorten).join('\n'))
    }
  }
  client.child.kill('SIGKILL')
}
process.exit(0)
